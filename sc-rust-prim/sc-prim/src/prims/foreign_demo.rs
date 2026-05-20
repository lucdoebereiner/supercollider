//! A Rust object owned by an sclang object.
//!
//! `Counter` lives on the Rust heap and is attached to a `RustCounter` instance.
//! Its `Drop` runs either eagerly (when `.free` is called) or at GC time (the
//! finalizer), printing so both paths are visible in the demo. No manual free.

use crate::{foreign, sc_primitive, Args, PrimError, Value};

/// A plain Rust type with real `Drop` cleanup.
pub struct Counter {
    label: String,
    count: i64,
}

impl Drop for Counter {
    fn drop(&mut self) {
        crate::log(&format!(
            "  [Rust Drop] Counter '{}' freed at count {}\n",
            self.label, self.count
        ));
    }
}

/// `RustCounter.new(label)` body: attach a fresh `Counter` to `self`.
pub fn counter_new(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let label = args.arg(1).as_str().unwrap_or("counter").to_string();
    unsafe { foreign::attach(args.vm(), obj, Counter { label, count: 0 }) };
    args.set_result(Value::Obj(obj)); // return self
    Ok(())
}
sc_primitive!(COUNTER_NEW, "_RustCounterNew", 2, counter_new);

/// `aRustCounter.next` -> increment and return the new count.
pub fn counter_next(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let count = unsafe {
        foreign::with_mut::<Counter, _>(obj, |c| {
            c.count += 1;
            c.count
        })
    }
    .ok_or(PrimError::FAILED)?;
    args.set_result(Value::Int(count as i32));
    Ok(())
}
sc_primitive!(COUNTER_NEXT, "_RustCounterNext", 1, counter_next);

/// `aRustCounter.free` -> drop the Rust object now (deterministic cleanup).
pub fn counter_free(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    unsafe { foreign::take::<Counter>(obj) }; // dropped here
    args.set_result(Value::Nil);
    Ok(())
}
sc_primitive!(COUNTER_FREE, "_RustCounterFree", 1, counter_free);
