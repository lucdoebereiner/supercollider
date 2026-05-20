//! Safe reading and writing of value slots.
//!
//! A [`Value`] is the Rust mirror of a tagged sclang slot. [`Slot`] is a
//! borrowed handle to one slot on the stack (or inside an object) that knows
//! its lifetime, so you cannot hold it past the primitive call.

use std::marker::PhantomData;

use crate::error::PrimError;
use crate::host::tags::*;
use crate::host::{sc_obj_is_string, sc_obj_size, sc_obj_slots, RawSlot, ScObj};

/// A decoded sclang value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Value {
    Nil,
    Bool(bool),
    Int(i32),
    Float(f64),
    Char(u8),
    /// A reference to a GC-owned object.
    Obj(*mut ScObj),
    /// A raw foreign pointer (sclang tagPtr).
    Ptr(*mut std::os::raw::c_void),
}

/// Decode a raw slot. Caller guarantees `s` points at a live, valid slot.
pub(crate) unsafe fn read_value(s: *const RawSlot) -> Value {
    let r = &*s;
    match r.tag {
        TAG_NIL => Value::Nil,
        TAG_TRUE => Value::Bool(true),
        TAG_FALSE => Value::Bool(false),
        TAG_INT => Value::Int(r.u.i as i32),
        TAG_CHAR => Value::Char(r.u.i as u8),
        TAG_FLOAT => Value::Float(r.u.f),
        TAG_OBJ => Value::Obj(r.u.ptr as *mut ScObj),
        TAG_PTR => Value::Ptr(r.u.ptr),
        // tagSym / tagNotInit and anything unexpected: surface as Nil.
        _ => Value::Nil,
    }
}

/// Encode a value into a raw slot. Caller guarantees `s` is writable.
///
/// Note: storing an `Obj`/`Ptr` here does NOT run a write barrier — that is the
/// job of the array/object builders in [`crate::gc`]. Writing into a stack slot
/// (the primitive result) never needs a barrier because the stack is a GC root.
pub(crate) unsafe fn write_value(s: *mut RawSlot, v: Value) {
    let r = &mut *s;
    match v {
        Value::Nil => {
            r.tag = TAG_NIL;
            r.u.i = 0;
        }
        Value::Bool(true) => {
            r.tag = TAG_TRUE;
            r.u.i = 0;
        }
        Value::Bool(false) => {
            r.tag = TAG_FALSE;
            r.u.i = 0;
        }
        Value::Int(x) => {
            r.tag = TAG_INT;
            r.u.i = x as i64;
        }
        Value::Char(c) => {
            r.tag = TAG_CHAR;
            r.u.i = c as i64;
        }
        Value::Float(x) => {
            r.tag = TAG_FLOAT;
            r.u.f = x;
        }
        Value::Obj(o) => {
            r.tag = TAG_OBJ;
            r.u.ptr = o as *mut std::os::raw::c_void;
        }
        Value::Ptr(p) => {
            r.tag = TAG_PTR;
            r.u.ptr = p;
        }
    }
}

/// A borrowed handle to one slot, valid only for the primitive call `'a`.
pub struct Slot<'a> {
    pub(crate) ptr: *mut RawSlot,
    pub(crate) _pd: PhantomData<&'a mut RawSlot>,
}

impl<'a> Slot<'a> {
    /// The decoded value.
    pub fn value(&self) -> Value {
        unsafe { read_value(self.ptr) }
    }

    /// Read as an integer, or `WRONG_TYPE`.
    pub fn as_int(&self) -> Result<i32, PrimError> {
        match self.value() {
            Value::Int(i) => Ok(i),
            _ => Err(PrimError::WRONG_TYPE),
        }
    }

    /// Read as a float, accepting ints (sclang's usual numeric coercion).
    pub fn as_float(&self) -> Result<f64, PrimError> {
        match self.value() {
            Value::Float(f) => Ok(f),
            Value::Int(i) => Ok(i as f64),
            _ => Err(PrimError::WRONG_TYPE),
        }
    }

    /// Read as an object reference, or `WRONG_TYPE`.
    pub fn as_obj(&self) -> Result<*mut ScObj, PrimError> {
        match self.value() {
            Value::Obj(o) => Ok(o),
            _ => Err(PrimError::WRONG_TYPE),
        }
    }

    /// Overwrite this slot with `v`.
    pub fn set(&self, v: Value) {
        unsafe { write_value(self.ptr, v) }
    }

    /// Borrow the bytes of a `String` argument. Errors unless the slot really
    /// holds a String object.
    pub fn as_bytes(&self) -> Result<&'a [u8], PrimError> {
        let o = self.as_obj()?;
        unsafe {
            if sc_obj_is_string(o) == 0 {
                return Err(PrimError::WRONG_TYPE);
            }
            let len = sc_obj_size(o).max(0) as usize;
            let data = sc_obj_slots(o) as *const u8;
            Ok(std::slice::from_raw_parts(data, len))
        }
    }

    /// Borrow a `String` argument as `&str` (UTF-8 validated).
    pub fn as_str(&self) -> Result<&'a str, PrimError> {
        std::str::from_utf8(self.as_bytes()?).map_err(|_| PrimError::WRONG_TYPE)
    }

    /// Borrow the elements of an `Array` (slot-format) argument as raw slots.
    /// Use [`read_value`] / [`Value`] to interpret each element.
    pub fn obj_slots(&self) -> Result<&'a [RawSlot], PrimError> {
        let o = self.as_obj()?;
        unsafe {
            let len = sc_obj_size(o).max(0) as usize;
            let data = sc_obj_slots(o);
            Ok(std::slice::from_raw_parts(data, len))
        }
    }

    /// Convenience: read an `Array` of numbers as `f64`s (ints coerced).
    /// Non-numeric elements become `0.0`.
    pub fn as_f64_vec(&self) -> Result<Vec<f64>, PrimError> {
        let slots = self.obj_slots()?;
        Ok(slots
            .iter()
            .map(|s| match unsafe { read_value(s as *const RawSlot) } {
                Value::Float(f) => f,
                Value::Int(i) => i as f64,
                _ => 0.0,
            })
            .collect())
    }
}
