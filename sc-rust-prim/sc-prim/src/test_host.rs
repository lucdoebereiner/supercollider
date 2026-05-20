//! A minimal in-process implementation of the `sc_host.h` ABI, used only by
//! `cargo test`. It lets the real primitive code run end-to-end (stack, object
//! allocation, finalizers) without sclang. Objects are leaked; finalizers are
//! run on demand to simulate a collection.

#![cfg(test)]

use std::cell::RefCell;
use std::os::raw::{c_char, c_void};

use crate::host::tags::TAG_NOT_INIT;
use crate::host::{RawSlot, ScFinalizerFn, ScObj, ScPrimFn, ScVm, SlotUnion};

struct TObj {
    size: i32,
    is_string: i32,
    slots: Vec<RawSlot>, // for arrays (8-byte aligned)
    bytes: Vec<u8>,      // for strings
}

thread_local! {
    static STACK: RefCell<Vec<RawSlot>> = const { RefCell::new(Vec::new()) };
    static POSTS: RefCell<String> = const { RefCell::new(String::new()) };
    static FINALIZERS: RefCell<Vec<(ScFinalizerFn, *mut ScObj)>> = const { RefCell::new(Vec::new()) };
}

fn empty_slot() -> RawSlot {
    RawSlot {
        tag: TAG_NOT_INIT,
        u: SlotUnion { i: 0 },
    }
}

// ---- test helpers --------------------------------------------------------

pub fn vm_ptr() -> *mut ScVm {
    // Never dereferenced by this backend.
    core::ptr::NonNull::<ScVm>::dangling().as_ptr()
}

pub fn set_stack(slots: Vec<RawSlot>) {
    STACK.with(|s| *s.borrow_mut() = slots);
}

pub fn stack_slot(i: usize) -> RawSlot {
    STACK.with(|s| s.borrow()[i])
}

pub fn posts() -> String {
    POSTS.with(|p| p.borrow().clone())
}

pub fn clear_posts() {
    POSTS.with(|p| p.borrow_mut().clear());
}

pub fn make_array(size: i32) -> *mut ScObj {
    new_obj(TObj {
        size,
        is_string: 0,
        slots: vec![empty_slot(); size.max(0) as usize],
        bytes: Vec::new(),
    })
}

pub fn make_string(s: &str) -> *mut ScObj {
    new_obj(TObj {
        size: s.len() as i32,
        is_string: 1,
        slots: Vec::new(),
        bytes: s.as_bytes().to_vec(),
    })
}

pub fn run_all_finalizers() {
    let list = FINALIZERS.with(|f| f.borrow().clone());
    let g = vm_ptr();
    for (func, obj) in list {
        func(g, obj);
    }
}

fn new_obj(o: TObj) -> *mut ScObj {
    Box::into_raw(Box::new(o)) as *mut ScObj
}

fn slots_ptr(o: *mut ScObj) -> *mut RawSlot {
    unsafe {
        let t = &mut *(o as *mut TObj);
        if t.is_string != 0 {
            t.bytes.as_mut_ptr() as *mut RawSlot
        } else {
            t.slots.as_mut_ptr()
        }
    }
}

// ---- the sc_host.h ABI ---------------------------------------------------

#[no_mangle]
pub extern "C" fn sc_define_primitive(_name: *const c_char, _f: ScPrimFn, _n: i32, _v: i32) {}

#[no_mangle]
pub extern "C" fn sc_stack_ptr(_g: *mut ScVm) -> *mut RawSlot {
    STACK.with(|s| {
        let mut b = s.borrow_mut();
        let n = b.len();
        unsafe { b.as_mut_ptr().add(n - 1) }
    })
}

#[no_mangle]
pub extern "C" fn sc_new_array(_g: *mut ScVm, size: i32) -> *mut ScObj {
    make_array(size)
}

#[no_mangle]
pub extern "C" fn sc_new_string(_g: *mut ScVm, bytes: *const u8, len: i32) -> *mut ScObj {
    let slice = unsafe { std::slice::from_raw_parts(bytes, len.max(0) as usize) };
    new_obj(TObj {
        size: len,
        is_string: 1,
        slots: Vec::new(),
        bytes: slice.to_vec(),
    })
}

#[no_mangle]
pub extern "C" fn sc_obj_slots(o: *mut ScObj) -> *mut RawSlot {
    slots_ptr(o)
}

#[no_mangle]
pub extern "C" fn sc_obj_size(o: *mut ScObj) -> i32 {
    unsafe { (*(o as *mut TObj)).size }
}

#[no_mangle]
pub extern "C" fn sc_obj_set_size(o: *mut ScObj, n: i32) {
    unsafe { (*(o as *mut TObj)).size = n }
}

#[no_mangle]
pub extern "C" fn sc_obj_is_string(o: *mut ScObj) -> i32 {
    unsafe { (*(o as *mut TObj)).is_string }
}

#[no_mangle]
pub extern "C" fn sc_gc_write(_g: *mut ScVm, _parent: *mut ScObj, _slot: *mut RawSlot) {}

#[no_mangle]
pub extern "C" fn sc_gc_enter_delayed(_g: *mut ScVm) {}

#[no_mangle]
pub extern "C" fn sc_gc_exit_delayed(_g: *mut ScVm) {}

#[no_mangle]
pub extern "C" fn sc_install_finalizer(_g: *mut ScVm, obj: *mut ScObj, _idx: i32, f: ScFinalizerFn) {
    FINALIZERS.with(|fz| fz.borrow_mut().push((f, obj)));
}

#[no_mangle]
pub extern "C" fn sc_post(msg: *const c_char) {
    let s = unsafe { std::ffi::CStr::from_ptr(msg) }.to_string_lossy().into_owned();
    POSTS.with(|p| p.borrow_mut().push_str(&s));
}

// Silence "unused" for the c_void import on some toolchains.
const _: Option<*mut c_void> = None;
