//! Raw FFI surface: the `sc_host.h` C ABI plus the few constants that must
//! match sclang. This is the *only* place that talks to the host directly;
//! everything above it (`slot`, `args`, `gc`, `foreign`) is safe.

use std::os::raw::{c_char, c_void};

/// Opaque handle to sclang's `VMGlobals`.
#[repr(C)]
pub struct ScVm {
    _private: [u8; 0],
}

/// Opaque handle to a `PyrObject`.
#[repr(C)]
pub struct ScObj {
    _private: [u8; 0],
}

/// The 8-byte payload of a slot. Binary-compatible with sclang's slot union.
#[repr(C)]
#[derive(Copy, Clone)]
pub union SlotUnion {
    pub i: i64,
    pub f: f64,
    pub ptr: *mut c_void,
}

/// A value slot: 8-byte tag + 8-byte union, matching `PyrSlot` (PyrSlot64.h).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct RawSlot {
    pub tag: i64,
    pub u: SlotUnion,
}

/// Matches `PrimitiveHandler` = `int(*)(VMGlobals*, int)`.
pub type ScPrimFn = extern "C" fn(*mut ScVm, i32) -> i32;

/// Matches `ObjFuncPtr` = `int(*)(VMGlobals*, PyrObject*)`.
pub type ScFinalizerFn = extern "C" fn(*mut ScVm, *mut ScObj) -> i32;

/// Slot tag values (lang/LangSource/PyrSlot64.h).
pub mod tags {
    pub const TAG_NOT_INIT: i64 = 0;
    pub const TAG_OBJ: i64 = 1;
    pub const TAG_INT: i64 = 2;
    pub const TAG_SYM: i64 = 3;
    pub const TAG_CHAR: i64 = 4;
    pub const TAG_NIL: i64 = 5;
    pub const TAG_FALSE: i64 = 6;
    pub const TAG_TRUE: i64 = 7;
    pub const TAG_PTR: i64 = 8;
    pub const TAG_FLOAT: i64 = 9;
}

/// VM error codes (lang/LangSource/PyrErrors.h).
pub mod errors {
    pub const ERR_NONE: i32 = 0;
    pub const ERR_FAILED: i32 = 5000;
    pub const ERR_WRONG_TYPE: i32 = 5002;
    pub const ERR_INDEX_OUT_OF_RANGE: i32 = 5004;
    pub const ERR_OUT_OF_MEMORY: i32 = 5008;
}

extern "C" {
    pub fn sc_define_primitive(name: *const c_char, func: ScPrimFn, num_args: i32, var_args: i32);
    pub fn sc_stack_ptr(g: *mut ScVm) -> *mut RawSlot;
    pub fn sc_new_array(g: *mut ScVm, size: i32) -> *mut ScObj;
    pub fn sc_new_string(g: *mut ScVm, bytes: *const u8, len: i32) -> *mut ScObj;
    pub fn sc_obj_slots(o: *mut ScObj) -> *mut RawSlot;
    pub fn sc_obj_size(o: *mut ScObj) -> i32;
    pub fn sc_obj_set_size(o: *mut ScObj, n: i32);
    pub fn sc_obj_is_string(o: *mut ScObj) -> i32;
    pub fn sc_gc_write(g: *mut ScVm, parent: *mut ScObj, slot: *mut RawSlot);
    pub fn sc_gc_enter_delayed(g: *mut ScVm);
    pub fn sc_gc_exit_delayed(g: *mut ScVm);
    pub fn sc_install_finalizer(g: *mut ScVm, obj: *mut ScObj, slot_index: i32, func: ScFinalizerFn);
    pub fn sc_post(msg: *const c_char);
}
