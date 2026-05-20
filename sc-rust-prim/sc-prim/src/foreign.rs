//! Owning a Rust value from an sclang object.
//!
//! Pattern: a Rust value is boxed onto the Rust heap; its pointer is stashed in
//! instance-var slot [`PTR_SLOT`] of an sclang object, and a finalizer is
//! installed in slot [`FINALIZER_SLOT`]. When the sclang object is collected,
//! the finalizer runs and drops the box — so the Rust value's `Drop` impl fires
//! at GC time. You never call `free`.
//!
//! For deterministic cleanup, expose an explicit method that calls [`take`],
//! which drops eagerly and nils the pointer; the finalizer then no-ops, so
//! there is no double free.
//!
//! Convention enforced here: slot 0 = pointer, slot 1 = finalizer. Your `.sc`
//! class must declare (at least) those two instance variables first.

use std::os::raw::c_void;

use crate::host::errors::ERR_NONE;
use crate::host::tags::{TAG_NIL, TAG_PTR};
use crate::host::{sc_install_finalizer, sc_obj_slots, ScObj, ScVm};

/// Instance-var slot index holding the boxed Rust pointer.
pub const PTR_SLOT: usize = 0;
/// Instance-var slot index holding the finalizer reference.
pub const FINALIZER_SLOT: usize = 1;

/// Move `value` into the sclang object's ownership.
///
/// # Safety
/// `obj` must be a live sclang object with at least two instance-var slots.
pub unsafe fn attach<T>(g: *mut ScVm, obj: *mut ScObj, value: T) {
    let boxed = Box::into_raw(Box::new(value)) as *mut c_void;
    let s = sc_obj_slots(obj).add(PTR_SLOT);
    (*s).tag = TAG_PTR;
    (*s).u.ptr = boxed;
    sc_install_finalizer(g, obj, FINALIZER_SLOT as i32, drop_finalizer::<T>);
}

/// Borrow the boxed value mutably and run `f` on it. `None` if nothing attached.
///
/// # Safety
/// `obj` must be the object an earlier [`attach::<T>`] wrote to.
pub unsafe fn with_mut<T, R>(obj: *mut ScObj, f: impl FnOnce(&mut T) -> R) -> Option<R> {
    let s = sc_obj_slots(obj).add(PTR_SLOT);
    if (*s).tag != TAG_PTR || (*s).u.ptr.is_null() {
        return None;
    }
    let p = (*s).u.ptr as *mut T;
    Some(f(&mut *p))
}

/// Reclaim the boxed value eagerly, nilling the pointer slot so the finalizer
/// becomes a no-op. The returned box drops at the end of the calling scope.
///
/// # Safety
/// `obj` must be the object an earlier [`attach::<T>`] wrote to.
pub unsafe fn take<T>(obj: *mut ScObj) -> Option<Box<T>> {
    let s = sc_obj_slots(obj).add(PTR_SLOT);
    if (*s).tag != TAG_PTR || (*s).u.ptr.is_null() {
        return None;
    }
    let p = (*s).u.ptr as *mut T;
    (*s).tag = TAG_NIL;
    (*s).u.i = 0;
    Some(Box::from_raw(p))
}

/// The finalizer installed by [`attach`]. Drops the boxed `T` if still present.
pub extern "C" fn drop_finalizer<T>(_g: *mut ScVm, obj: *mut ScObj) -> i32 {
    unsafe {
        let _ = take::<T>(obj);
    }
    ERR_NONE
}
