//! End-to-end tests that drive the real primitive wrappers against the
//! in-process `test_host` backend.

#![cfg(test)]

use crate::host::tags::*;
use crate::host::{
    sc_obj_float_data, sc_obj_is_string, sc_obj_size, sc_obj_slots, RawSlot, ScObj, SlotUnion,
};
use crate::prims::{array, foreign_demo, math, signal, string};
use crate::test_host;
use crate::PrimDesc;
use std::sync::atomic::{AtomicI64, Ordering::SeqCst};

fn islot(i: i64) -> RawSlot {
    RawSlot { tag: TAG_INT, u: SlotUnion { i } }
}
fn fslot(f: f64) -> RawSlot {
    RawSlot { tag: TAG_FLOAT, u: SlotUnion { f } }
}
fn oslot(o: *mut ScObj) -> RawSlot {
    RawSlot {
        tag: TAG_OBJ,
        u: SlotUnion { ptr: o as *mut std::os::raw::c_void },
    }
}
fn nil() -> RawSlot {
    RawSlot { tag: TAG_NIL, u: SlotUnion { i: 0 } }
}

/// Push `stack` (stack[0] = receiver), call the primitive, return the result
/// slot (which the convention writes back into the receiver slot, index 0).
fn call(desc: PrimDesc, stack: Vec<RawSlot>) -> RawSlot {
    let n = stack.len() as i32;
    test_host::set_stack(stack);
    let err = (desc.func)(test_host::vm_ptr(), n);
    assert_eq!(err, 0, "primitive {} returned error {}", desc.name, err);
    test_host::stack_slot(0)
}

fn obj_of(slot: RawSlot) -> *mut ScObj {
    assert_eq!(slot.tag, TAG_OBJ);
    unsafe { slot.u.ptr as *mut ScObj }
}

fn read_ints(o: *mut ScObj) -> Vec<i64> {
    unsafe {
        let n = sc_obj_size(o) as usize;
        std::slice::from_raw_parts(sc_obj_slots(o), n)
            .iter()
            .map(|s| s.u.i)
            .collect()
    }
}

fn read_string(o: *mut ScObj) -> String {
    unsafe {
        assert_eq!(sc_obj_is_string(o), 1);
        let n = sc_obj_size(o) as usize;
        let bytes = std::slice::from_raw_parts(sc_obj_slots(o) as *const u8, n);
        String::from_utf8_lossy(bytes).into_owned()
    }
}

#[test]
fn nth_prime_returns_int() {
    let r = call(math::NTH_PRIME, vec![nil(), islot(10)]);
    assert_eq!(r.tag, TAG_INT);
    assert_eq!(unsafe { r.u.i }, 29);
}

#[test]
fn nth_prime_zero_is_nil() {
    let r = call(math::NTH_PRIME, vec![nil(), islot(0)]);
    assert_eq!(r.tag, TAG_NIL);
}

#[test]
fn hypot_returns_float() {
    let r = call(math::HYPOT, vec![nil(), fslot(3.0), fslot(4.0)]);
    assert_eq!(r.tag, TAG_FLOAT);
    assert!((unsafe { r.u.f } - 5.0).abs() < 1e-12);
}

#[test]
fn hypot_accepts_ints() {
    let r = call(math::HYPOT, vec![nil(), islot(3), islot(4)]);
    assert!((unsafe { r.u.f } - 5.0).abs() < 1e-12);
}

#[test]
fn wrong_type_is_reported() {
    // hypot on nil should fail with errWrongType, not panic.
    test_host::set_stack(vec![nil(), nil(), nil()]);
    let err = (math::HYPOT.func)(test_host::vm_ptr(), 3);
    assert_eq!(err, crate::host::errors::ERR_WRONG_TYPE);
}

#[test]
fn primes_up_to_builds_array() {
    let r = call(array::PRIMES_UP_TO, vec![nil(), islot(10)]);
    assert_eq!(read_ints(obj_of(r)), vec![2, 3, 5, 7]);
}

#[test]
fn histogram_bins() {
    let data = test_host::make_array(4);
    unsafe {
        let s = sc_obj_slots(data);
        *s.add(0) = fslot(0.0);
        *s.add(1) = fslot(0.1);
        *s.add(2) = fslot(0.9);
        *s.add(3) = fslot(1.0);
    }
    let r = call(array::HISTOGRAM, vec![nil(), oslot(data), islot(2)]);
    // two low, two high
    assert_eq!(read_ints(obj_of(r)), vec![2, 2]);
}

#[test]
fn reverse_string() {
    let s = test_host::make_string("hello");
    let r = call(string::REVERSE, vec![nil(), oslot(s)]);
    assert_eq!(read_string(obj_of(r)), "olleh");
}

#[test]
fn shout_string() {
    let s = test_host::make_string("hi");
    let r = call(string::SHOUT, vec![nil(), oslot(s)]);
    assert_eq!(read_string(obj_of(r)), "HI!");
}

#[test]
fn foreign_counter_explicit_free() {
    let inst = test_host::make_array(2); // 2 instance-var slots
    let label = test_host::make_string("c1");
    call(foreign_demo::COUNTER_NEW, vec![oslot(inst), oslot(label)]);

    assert_eq!(unsafe { call(foreign_demo::COUNTER_NEXT, vec![oslot(inst)]).u.i }, 1);
    assert_eq!(unsafe { call(foreign_demo::COUNTER_NEXT, vec![oslot(inst)]).u.i }, 2);
    assert_eq!(unsafe { call(foreign_demo::COUNTER_NEXT, vec![oslot(inst)]).u.i }, 3);

    test_host::clear_posts();
    call(foreign_demo::COUNTER_FREE, vec![oslot(inst)]);
    assert!(
        test_host::posts().contains("freed at count 3"),
        "Drop should run eagerly on free; posts = {:?}",
        test_host::posts()
    );

    // Finalizer must NOT double-free after an explicit free.
    test_host::clear_posts();
    test_host::run_all_finalizers();
    assert!(!test_host::posts().contains("freed"));
}

#[test]
fn foreign_counter_finalizer_path() {
    let inst = test_host::make_array(2);
    let label = test_host::make_string("c2");
    call(foreign_demo::COUNTER_NEW, vec![oslot(inst), oslot(label)]);
    call(foreign_demo::COUNTER_NEXT, vec![oslot(inst)]);

    // No explicit free: collection triggers the finalizer -> Rust Drop.
    test_host::clear_posts();
    test_host::run_all_finalizers();
    assert!(
        test_host::posts().contains("Counter 'c2' freed at count 1"),
        "finalizer should drop the Counter; posts = {:?}",
        test_host::posts()
    );
}

// ---- Signal primitives ---------------------------------------------------

fn read_floats(o: *mut ScObj) -> Vec<f32> {
    unsafe {
        let n = sc_obj_size(o) as usize;
        std::slice::from_raw_parts(sc_obj_float_data(o), n).to_vec()
    }
}

#[test]
fn sine_signal_creates_one_cycle() {
    let r = call(signal::SINE, vec![nil(), islot(4)]);
    let s = read_floats(obj_of(r));
    assert_eq!(s.len(), 4);
    // sin(0), sin(pi/2), sin(pi), sin(3pi/2) ~ 0, 1, 0, -1
    let expect = [0.0, 1.0, 0.0, -1.0];
    for (got, want) in s.iter().zip(expect) {
        assert!((got - want).abs() < 1e-6, "got {s:?}");
    }
}

#[test]
fn normalize_signal_scales_to_peak_one() {
    let input = test_host::make_signal(&[0.0, 0.25, -0.5, 0.1]);
    let r = call(signal::NORMALIZE, vec![oslot(input)]);
    let out = read_floats(obj_of(r));
    assert_eq!(out, vec![0.0, 0.5, -1.0, 0.2]); // divided by peak 0.5
}

#[test]
fn rms_of_signal() {
    let input = test_host::make_signal(&[1.0, -1.0, 1.0, -1.0]);
    let r = call(signal::RMS, vec![oslot(input)]);
    assert_eq!(r.tag, TAG_FLOAT);
    assert!((unsafe { r.u.f } - 1.0).abs() < 1e-9);
}

#[test]
fn factorize_number() {
    let r = call(math::FACTORIZE, vec![nil(), islot(360)]);
    assert_eq!(read_ints(obj_of(r)), vec![2, 2, 2, 3, 3, 5]); // 360 = 2^3 * 3^2 * 5
}

// ---- memory-safety tests -------------------------------------------------
//
// A `LeakProbe` increments a caller-owned counter on creation and decrements it
// on `Drop`. Driving the foreign-object machinery (attach / take / finalizer)
// with it lets us assert that every Rust value handed to sclang is freed exactly
// once: the counter must return to zero (no leak) and never go negative (no
// double free). Each test owns its own counter, so the suite stays parallel-safe.

struct LeakProbe {
    counter: *const AtomicI64,
}
impl LeakProbe {
    fn new(counter: &AtomicI64) -> Self {
        counter.fetch_add(1, SeqCst);
        LeakProbe { counter }
    }
}
impl Drop for LeakProbe {
    fn drop(&mut self) {
        unsafe { (*self.counter).fetch_sub(1, SeqCst) };
    }
}

#[test]
fn no_leak_on_explicit_free() {
    let live = AtomicI64::new(0);
    for _ in 0..10_000 {
        let obj = test_host::make_array(2);
        unsafe { crate::foreign::attach(test_host::vm_ptr(), obj, LeakProbe::new(&live)) };
        assert_eq!(live.load(SeqCst), 1);
        unsafe { crate::foreign::take::<LeakProbe>(obj) }; // dropped here
        assert_eq!(live.load(SeqCst), 0);
    }
    assert_eq!(live.load(SeqCst), 0, "explicit free leaked");
}

#[test]
fn no_leak_on_finalizer_collection() {
    let live = AtomicI64::new(0);
    let objs: Vec<*mut ScObj> = (0..10_000)
        .map(|_| {
            let obj = test_host::make_array(2);
            unsafe { crate::foreign::attach(test_host::vm_ptr(), obj, LeakProbe::new(&live)) };
            obj
        })
        .collect();
    assert_eq!(live.load(SeqCst), 10_000);
    // Simulate the GC collecting every object: each runs its finalizer once.
    for &obj in &objs {
        crate::foreign::drop_finalizer::<LeakProbe>(test_host::vm_ptr(), obj);
    }
    assert_eq!(live.load(SeqCst), 0, "finalizer path leaked");
}

#[test]
fn no_double_free() {
    let live = AtomicI64::new(0);
    let obj = test_host::make_array(2);
    unsafe { crate::foreign::attach(test_host::vm_ptr(), obj, LeakProbe::new(&live)) };
    unsafe { crate::foreign::take::<LeakProbe>(obj) }; // free once
    assert_eq!(live.load(SeqCst), 0);
    // The finalizer (and any further take) must be no-ops, not a second drop.
    crate::foreign::drop_finalizer::<LeakProbe>(test_host::vm_ptr(), obj);
    unsafe { crate::foreign::take::<LeakProbe>(obj) };
    assert_eq!(live.load(SeqCst), 0, "double free detected (counter went negative)");
}
