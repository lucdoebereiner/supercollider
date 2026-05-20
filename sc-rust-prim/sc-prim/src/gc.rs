//! Safe object construction.
//!
//! [`Gc`] is an RAII token: creating it enters sclang's "delayed collection"
//! context, and dropping it leaves that context. While you hold it, no
//! collection runs, so freshly allocated objects can't be reclaimed mid-build —
//! the classic footgun of C++ primitives disappears by construction.
//!
//! Every slot written through [`ArrayBuilder::set`] runs the GC write barrier
//! for you, so the *other* classic footgun (a forgotten `GCWrite`) is also gone.

use std::marker::PhantomData;

use crate::error::PrimError;
use crate::host::{
    sc_gc_enter_delayed, sc_gc_exit_delayed, sc_gc_write, sc_new_array, sc_new_signal,
    sc_new_string, sc_obj_float_data, sc_obj_set_size, sc_obj_slots, ScObj, ScVm,
};
use crate::slot::{write_value, Value};

/// A delayed-collection scope. Allocate objects through it; it cannot outlive
/// the primitive call.
pub struct Gc {
    g: *mut ScVm,
}

impl Gc {
    /// # Safety
    /// `g` must be the current VM handle for this primitive call.
    pub(crate) unsafe fn new(g: *mut ScVm) -> Self {
        sc_gc_enter_delayed(g);
        Gc { g }
    }

    /// The VM handle.
    pub fn vm(&self) -> *mut ScVm {
        self.g
    }

    /// Allocate an empty `Array` of `len` slots, ready to be filled.
    pub fn new_array(&self, len: usize) -> Result<ArrayBuilder<'_>, PrimError> {
        let obj = unsafe { sc_new_array(self.g, len as i32) };
        if obj.is_null() {
            return Err(PrimError::OUT_OF_MEMORY);
        }
        unsafe { sc_obj_set_size(obj, len as i32) };
        Ok(ArrayBuilder {
            g: self.g,
            obj,
            len,
            _pd: PhantomData,
        })
    }

    /// Allocate a `String` holding `s`.
    pub fn new_string(&self, s: &str) -> Result<Value, PrimError> {
        let bytes = s.as_bytes();
        let obj = unsafe { sc_new_string(self.g, bytes.as_ptr(), bytes.len() as i32) };
        if obj.is_null() {
            return Err(PrimError::OUT_OF_MEMORY);
        }
        Ok(Value::Obj(obj))
    }

    /// Allocate an empty `Signal` of `len` samples, ready to be filled.
    pub fn new_signal(&self, len: usize) -> Result<SignalBuilder<'_>, PrimError> {
        let obj = unsafe { sc_new_signal(self.g, len as i32) };
        if obj.is_null() {
            return Err(PrimError::OUT_OF_MEMORY);
        }
        unsafe { sc_obj_set_size(obj, len as i32) };
        let data = unsafe { sc_obj_float_data(obj) };
        Ok(SignalBuilder {
            obj,
            data,
            len,
            _pd: PhantomData,
        })
    }
}

impl Drop for Gc {
    fn drop(&mut self) {
        unsafe { sc_gc_exit_delayed(self.g) }
    }
}

/// A freshly allocated array being filled in. Borrows its [`Gc`] scope so it
/// cannot outlive the delayed-collection window.
pub struct ArrayBuilder<'a> {
    g: *mut ScVm,
    obj: *mut ScObj,
    len: usize,
    _pd: PhantomData<&'a Gc>,
}

impl<'a> ArrayBuilder<'a> {
    /// Number of slots.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Store `v` at index `i` (no-op if out of range). The write barrier is
    /// applied automatically.
    pub fn set(&mut self, i: usize, v: Value) {
        if i >= self.len {
            return;
        }
        unsafe {
            let slot = sc_obj_slots(self.obj).add(i);
            write_value(slot, v);
            // Harmless for white objects, required once the array is reachable.
            sc_gc_write(self.g, self.obj, slot);
        }
    }

    /// Finish building and yield the array as a [`Value`] to return.
    pub fn finish(self) -> Value {
        Value::Obj(self.obj)
    }
}

/// A freshly allocated [`Signal`](https://doc.sccode.org/Classes/Signal.html)
/// being filled in. Samples are plain `f32`s, so no write barrier is needed
/// (floats are not object references).
pub struct SignalBuilder<'a> {
    obj: *mut ScObj,
    data: *mut f32,
    len: usize,
    _pd: PhantomData<&'a Gc>,
}

impl<'a> SignalBuilder<'a> {
    /// Number of samples.
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Store sample `v` at index `i` (no-op if out of range).
    pub fn set(&mut self, i: usize, v: f32) {
        if i < self.len {
            unsafe { *self.data.add(i) = v };
        }
    }

    /// The samples as a mutable slice — fill it however you like.
    pub fn as_mut_slice(&mut self) -> &mut [f32] {
        unsafe { std::slice::from_raw_parts_mut(self.data, self.len) }
    }

    /// Finish building and yield the Signal as a [`Value`] to return.
    pub fn finish(self) -> Value {
        Value::Obj(self.obj)
    }
}
