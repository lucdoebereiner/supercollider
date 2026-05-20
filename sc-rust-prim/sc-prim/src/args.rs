//! The argument view passed to every primitive.
//!
//! sclang pushes the receiver and arguments onto a stack before calling a
//! primitive. For `N` args pushed, the receiver sits at `sp-(N-1)` and the
//! explicit arguments follow it up to `sp`. The receiver slot is also where the
//! return value goes. [`Args`] hides that arithmetic behind safe accessors.

use std::marker::PhantomData;

use crate::error::PrimError;
use crate::host::{RawSlot, ScObj, ScVm};
use crate::slot::{write_value, Slot, Value};

/// Borrowed access to a primitive's arguments and result slot.
pub struct Args<'a> {
    g: *mut ScVm,
    sp: *mut RawSlot,
    num_args: i32,
    _pd: PhantomData<&'a mut RawSlot>,
}

impl<'a> Args<'a> {
    /// # Safety
    /// `sp` must be sclang's stack pointer and `num_args` the count it pushed.
    pub(crate) unsafe fn new(g: *mut ScVm, sp: *mut RawSlot, num_args: i32) -> Self {
        Args {
            g,
            sp,
            num_args,
            _pd: PhantomData,
        }
    }

    /// Number of slots pushed (receiver + explicit args).
    pub fn num_args(&self) -> i32 {
        self.num_args
    }

    fn receiver_ptr(&self) -> *mut RawSlot {
        // receiver = sp - (num_args - 1)
        unsafe { self.sp.offset(-(self.num_args as isize - 1)) }
    }

    /// The receiver (`self`). For a class method, this is the class.
    pub fn receiver(&self) -> Slot<'a> {
        Slot {
            ptr: self.receiver_ptr(),
            _pd: PhantomData,
        }
    }

    /// Argument `i`, where `arg(0)` is the receiver and `arg(1)` is the first
    /// explicit argument. Out-of-range indices are clamped to the last slot.
    pub fn arg(&self, i: i32) -> Slot<'a> {
        let i = i.clamp(0, (self.num_args - 1).max(0));
        Slot {
            ptr: unsafe { self.receiver_ptr().offset(i as isize) },
            _pd: PhantomData,
        }
    }

    /// The receiver as an object handle (for instance/foreign-object methods).
    pub fn receiver_obj(&self) -> Result<*mut ScObj, PrimError> {
        self.receiver().as_obj()
    }

    /// The VM handle, needed for allocation / finalizer installation.
    pub fn vm(&self) -> *mut ScVm {
        self.g
    }

    /// Set the primitive's return value (written into the receiver slot).
    pub fn set_result(&mut self, v: Value) {
        unsafe { write_value(self.receiver_ptr(), v) }
    }
}
