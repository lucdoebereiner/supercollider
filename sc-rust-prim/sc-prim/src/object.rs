//! Keeping sclang references alive across primitive calls — "rooting".
//!
//! SuperCollider's GC has **no explicit root API**. An object lives only as long
//! as it is *reachable*: from a GC root (the interpreter's operand stack) or from
//! another live object. The collector scans the stack and the object graph — it
//! does **not** scan Rust memory.
//!
//! Consequences for a primitive:
//!
//! - **Within one call** you need do nothing: the receiver and arguments are on
//!   the stack (a root), the [`Gc`](crate::Gc) scope suspends collection while you
//!   build, and the value you return via [`Args::set_result`](crate::Args::set_result)
//!   lands on the stack too. Leaf primitives are safe with zero rooting work.
//!
//! - **Across calls** (a Rust value attached via [`crate::foreign`] that wants to
//!   remember an sclang object, a cache, etc.) you must NOT keep the only
//!   reference in Rust memory — the GC can't see it, so the object gets collected
//!   and your pointer dangles. Instead store the reference in an **instance-var
//!   slot of an object sclang already holds** (so it's reachable), using
//!   [`set_field`]. That is how you "root" it; the write barrier is applied for
//!   you. Keep the *slot index* on the Rust side, not the pointer.

use crate::host::{sc_gc_write, sc_obj_slots, ScObj, ScVm};
use crate::slot::{read_value, write_value, Value};

/// Store `value` into instance-var slot `index` of `obj`, applying the GC write
/// barrier. If `obj` is itself reachable from sclang, storing an object `value`
/// here **roots** it: it survives future collections for as long as `obj` does.
///
/// Use this (not a Rust field) to remember an sclang object past your call.
///
/// # Safety
/// `obj` must be a live sclang object with at least `index + 1` instance-var
/// slots (e.g. the receiver of an instance method, or a foreign-object instance).
pub unsafe fn set_field(g: *mut ScVm, obj: *mut ScObj, index: usize, value: Value) {
    let slot = sc_obj_slots(obj).add(index);
    write_value(slot, value);
    sc_gc_write(g, obj, slot);
}

/// Read instance-var slot `index` of `obj`.
///
/// # Safety
/// `obj` must be a live sclang object with at least `index + 1` instance-var slots.
pub unsafe fn get_field(obj: *mut ScObj, index: usize) -> Value {
    read_value(sc_obj_slots(obj).add(index))
}
