// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 shygosh <shygosh@proton.me>.
 */

static __read_mostly unsigned int sched_sss_smt_bias = 4;
static __read_mostly unsigned int sched_sss_llc_bias = 4;

#define SSS_FACTOR (SCHED_CAPACITY_SCALE >> 5)
#define SSS_MARGIN (SCHED_CAPACITY_SCALE >> 3)

static __cacheline_aligned atomic_t sss_rt_factor_bank[CONFIG_NR_CPUS];
static __read_mostly unsigned int sss_cpu_capacities[CONFIG_NR_CPUS];
static __read_mostly struct cpumask sss_hp_mask;
static __read_mostly bool sss_asymmetric;

struct sss_candidate {
	long factor;
	int cpu;
};

int sss_select_task_rq_fair(struct task_struct *p, int prev_cpu, int wake_flags)
{
	int this_cpu = raw_smp_processor_id(), cpu;
	int p_queued, p_affine = 0;
	long p_factor;
	const struct cpumask *prev_mask;
	struct cpumask *llc_mask = NULL;
	struct sched_domain *sd;
	struct rq *rq;

	struct sss_candidate best = {
		.cpu = prev_cpu,
		.factor = 0
	};
	struct sss_candidate curr;
	struct cpumask cpus;

	if (unlikely(!cpumask_and(&cpus, p->cpus_ptr, cpu_active_mask)))
		return cpumask_first(p->cpus_ptr);

	if (wake_flags & WF_TTWU) {
		int sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);
		int valid = cpumask_test_cpu(this_cpu, &cpus);

		record_wakee(p);

		/* For synchronized wakeup, just return this_cpu if its valid. */
		if (((wake_flags & WF_CURRENT_CPU) || sync) && valid)
			return this_cpu;

		p_affine = !wake_wide(p) && valid;
	}

	rcu_read_lock();
	sd = rcu_dereference(per_cpu(sd_llc, prev_cpu));
	if (likely(sd))
		llc_mask = sched_domain_span(sd);
	rcu_read_unlock();

	if (!p_affine)
		prev_mask = cpu_smt_mask(prev_cpu);
	p_factor = READ_ONCE(p->se.avg.util_est) & ~UTIL_AVG_UNCHANGED;
	p_queued = task_on_rq_queued(p) || current == p;

	for_each_cpu(cpu, &cpus) {
		curr.cpu = cpu;
		curr.factor = sss_cpu_capacities[cpu];
		rq = cpu_rq(cpu);

		/*
		 * Calculate the remaining capacity of this cpu by subtracting
		 * its true capacity with its CFS, RT, DL utilization.
		 */
		curr.factor -= READ_ONCE(rq->cfs.avg.util_est);
		curr.factor -= READ_ONCE(rq->avg_rt.util_avg);
		curr.factor -= READ_ONCE(rq->avg_dl.util_avg);

		/*
		 * Account @p's factor to simulate cpu remaining capacity
		 * if @p is enqueued on this cpu.
		 */
		if (p_queued)
			curr.factor -= (cpu != prev_cpu) ? p_factor : 0;
		else
			curr.factor -= p_factor;

		/*
		 * For exec wakeup, skip cache heuristics altogether since it
		 * won't benefit from it.
		 */
		if (wake_flags & WF_EXEC)
			goto out;

		/*
		 * Performing cache heuristics on a busy cpu is a bad idea.
		 */
		if (curr.factor < SSS_MARGIN)
			goto out;

		/*
		 * If @p prefers wake-affine bias to both prev_cpu and this_cpu.
		 */
		if (p_affine && (cpu == this_cpu || cpu == prev_cpu))
			curr.factor += SSS_FACTOR * 8;

		/*
		 * Otherwise bias to prev_cpu and its SMT siblings.
		 */
		if (!p_affine && (wake_flags & WF_TTWU))
			if (cpumask_test_cpu(cpu, prev_mask))
				curr.factor += SSS_FACTOR * (long)sched_sss_smt_bias;

		/*
		 * Penalize candidates that doesn't share LLC with prev_cpu.
		 */
		if (likely(llc_mask) && cpumask_test_cpu(cpu, llc_mask))
			curr.factor += SSS_FACTOR * (long)sched_sss_llc_bias;

out:
		/*
		 * The cpu with the highest remaining capacity, wins.
		 */
		if (curr.factor > best.factor)
			best = curr;
	}

	return best.cpu;
}

int sss_select_task_rq_rt(struct task_struct *p, int prev_cpu, int wake_flags)
{
	int p_queued = task_on_rq_queued(p), cpu;
	long p_factor = MAX_RT_PRIO - p->normal_prio;

	struct sss_candidate best = {
		.cpu = prev_cpu,
		.factor = UINT_MAX
	};
	struct sss_candidate curr;
	struct cpumask cpus;

	if (unlikely(!cpumask_and(&cpus, p->cpus_ptr, cpu_active_mask)))
		return cpumask_first(p->cpus_ptr);

	if (sss_asymmetric)
		if (likely(cpumask_intersects(&cpus, &sss_hp_mask)))
			cpumask_and(&cpus, &cpus, &sss_hp_mask);

	if (wake_flags & (WF_TTWU | WF_FORK)) {
		struct task_struct *curtsk, *dnrtsk;
		struct rq *rq;

		rcu_read_lock();
		rq = cpu_rq(prev_cpu);
		curtsk = READ_ONCE(rq->curr);
		dnrtsk = READ_ONCE(rq->donor);

		if (curtsk && rt_task(dnrtsk) &&
		    (curtsk->nr_cpus_allowed < 2 ||
		     dnrtsk->normal_prio <= p->normal_prio))
			cpumask_clear_cpu(prev_cpu, &cpus);
		rcu_read_unlock();
	}

	for_each_cpu(cpu, &cpus) {
		curr.cpu = cpu;
		curr.factor = atomic_read(&sss_rt_factor_bank[cpu]);

		/*
		 * Account @p's factor to simulate accumulated priority
		 * if @p is enqueued on this cpu.
		 */
		if (p_queued)
			curr.factor += (cpu != prev_cpu) ? p_factor : 0;
		else
			curr.factor += p_factor;

		/*
		 * The cpu with the lowest accumulated priority, wins.
		 */
		if (curr.factor < best.factor)
			best = curr;
	}

	return best.cpu;
}

void sss_rt_add_factor(int cpu, int normal_prio)
{
	atomic_add(MAX_RT_PRIO - normal_prio, &sss_rt_factor_bank[cpu]);
}

void sss_rt_sub_factor(int cpu, int normal_prio)
{
	atomic_sub(MAX_RT_PRIO - normal_prio, &sss_rt_factor_bank[cpu]);
}

void __init sched_sss_init(void)
{
	unsigned int lowest_cap = UINT_MAX;
	int cpu;

	struct cpumask tmp_lp_mask;

	for_each_cpu(cpu, cpu_present_mask) {
		sss_cpu_capacities[cpu] = arch_scale_cpu_capacity(cpu);

		if (sss_cpu_capacities[cpu] < lowest_cap) {
			cpumask_clear(&tmp_lp_mask);
			lowest_cap = sss_cpu_capacities[cpu];
		}

		if (sss_cpu_capacities[cpu] == lowest_cap)
			cpumask_set_cpu(cpu, &tmp_lp_mask);
	}

	for_each_cpu_andnot(cpu, cpu_present_mask, &tmp_lp_mask) {
		cpumask_set_cpu(cpu, &sss_hp_mask);
	}

	/*
	 * For x86 desktop, just assume lp cpu count never exceeds hp's.
	 * We're expecting explicit E-cores presence.
	 */
	if (cpumask_weight(&tmp_lp_mask) <= cpumask_weight(&sss_hp_mask))
		sss_asymmetric = true;
}

#ifdef CONFIG_SYSCTL
static int sss_maxval_eight = 8;
static const struct ctl_table sched_sss_sysctls[] = {
	{
		.procname	= "sched_sss_smt_bias",
		.data		= &sched_sss_smt_bias,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &sss_maxval_eight,
	},
	{
		.procname	= "sched_sss_llc_bias",
		.data		= &sched_sss_llc_bias,
		.maxlen		= sizeof(unsigned int),
		.mode		= 0644,
		.proc_handler	= proc_douintvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &sss_maxval_eight,
	},
};

static int __init sched_sss_sysctl_init(void)
{
	register_sysctl_init("kernel", sched_sss_sysctls);
	return 0;
}
late_initcall(sched_sss_sysctl_init);
#endif
