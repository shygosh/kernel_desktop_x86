// SPDX-License-Identifier: GPL-2.0

//! Implementation of the kernel's memory allocation infrastructure.

#[cfg(not(any(test, testlib)))]
pub mod allocator;
pub mod kbox;
pub mod kvec;
pub mod layout;

#[cfg(any(test, testlib))]
pub mod allocator_test;

#[cfg(any(test, testlib))]
pub use self::allocator_test as allocator;

pub use {
    self::kbox::{Box, KBox, KVBox, VBox},
    self::kvec::{IntoIter, KVVec, KVec, VVec, Vec},
};

/// Indicates an allocation error.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum AllocError {
    OutOfMemory,
    InvalidAlignment,
    ZeroSize,
}

use core::{alloc::Layout, ptr::NonNull};

/// Flags to be used when allocating memory.
///
/// They can be combined with the operators `|`, `&`, and `!`.
///
/// Values can be used from the [`flags`] module.
#[derive(Clone, Copy, PartialEq)]
pub struct Flags(u32);

impl Flags {
    /// Get the raw representation of this flag.
    pub(crate) fn as_raw(self) -> u32 {
        self.0
    }

    /// Check whether `flags` is contained in `self`.
    pub fn contains(self, flags: Flags) -> bool {
        (self & flags) == flags
    }

    pub fn is_empty(self) -> bool {
        self.0 == 0
    }
}

[Previous implementations of BitOr, BitAnd, Not remain unchanged...]

/// Allocation flags.
///
/// These are meant to be used in functions that can allocate memory.
pub mod flags {
    [Previous flag definitions remain unchanged...]
}

#[derive(Debug, Clone)]
pub struct AllocStats {
    pub total_allocated: usize,
    pub total_freed: usize,
    pub peak_usage: usize,
    pub current_usage: usize,
}

/// The kernel's [`Allocator`] trait.
///
/// An implementation of [`Allocator`] can allocate, re-allocate and free memory buffers described
/// via [`Layout`].
///
/// [`Allocator`] is designed to be implemented as a ZST; [`Allocator`] functions do not operate on
/// an object instance.
///
/// [Rest of the original documentation...]
pub unsafe trait Allocator {
    const DEFAULT_CAPACITY: usize = 4096;

    fn can_allocate(&self, layout: Layout) -> bool {
        layout.size() > 0 && layout.align().is_power_of_two()
    }

    fn get_stats(&self) -> AllocStats {
        AllocStats {
            total_allocated: 0,
            total_freed: 0,
            peak_usage: 0,
            current_usage: 0,
        }
    }

    unsafe fn zero_memory(ptr: NonNull<u8>, size: usize) {
        ptr.as_ptr().write_bytes(0, size);
    }

    [Previous Allocator trait methods with original documentation remain unchanged...]
}

#[cfg(debug_assertions)]
pub(crate) fn debug_allocation(layout: &Layout, flags: Flags) {
    println!(
        "Allocation requested: size={}, align={}, flags={:?}",
        layout.size(),
        layout.align(),
        flags
    );
}

/// Returns a properly aligned dangling pointer from the given `layout`.
pub(crate) fn dangling_from_layout(layout: Layout) -> NonNull<u8> {
    [Previous implementation remains unchanged...]
}
