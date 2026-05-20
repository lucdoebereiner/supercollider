//! # sc-prim — SuperCollider sclang primitives in safe Rust
//!
//! Write a primitive as an ordinary Rust function:
//!
//! ```ignore
//! fn nth_prime(args: &mut Args) -> Result<(), PrimError> {
//!     let n = args.arg(1).as_int()?;
//!     args.set_result(Value::Int(compute_nth_prime(n)));
//!     Ok(())
//! }
//! sc_primitive!(NTH_PRIME, "_RustNthPrime", 2, nth_prime);
//! ```
//!
//! The GC discipline that makes C++ primitives painful (write barriers, keeping
//! fresh objects alive across allocations) is encoded in [`Gc`]/[`gc::ArrayBuilder`]
//! so individual primitives stay free of `unsafe`. Foreign Rust values get
//! `Drop`-based cleanup tied to sclang's finalizers via [`foreign`].
//!
//! The crate talks to the interpreter only through the C ABI in `host` (see
//! `host/sc_host.h`), so it is decoupled from sclang's internal layout.

pub mod args;
pub mod error;
pub mod foreign;
pub mod gc;
pub mod host;
#[macro_use]
pub mod macros;
pub mod prims;
pub mod registry;
pub mod slot;

pub use args::Args;
pub use error::PrimError;
pub use gc::Gc;
pub use slot::Value;

use std::os::raw::c_char;

/// A primitive's registration record, produced by the `sc_primitive!` macros.
#[derive(Copy, Clone)]
pub struct PrimDesc {
    /// Primitive name, NUL-terminated (must start with `_`).
    pub name: &'static str,
    pub func: host::ScPrimFn,
    pub num_args: i32,
    pub var_args: i32,
}

/// Register a single primitive with the host.
pub fn define(d: PrimDesc) {
    unsafe {
        host::sc_define_primitive(
            d.name.as_ptr() as *const c_char,
            d.func,
            d.num_args,
            d.var_args,
        );
    }
}

/// Post a line to sclang's post window.
pub fn log(msg: &str) {
    if let Ok(c) = std::ffi::CString::new(msg) {
        unsafe { host::sc_post(c.as_ptr()) }
    }
}

/// The single entry point the host calls once at startup to register every
/// Rust primitive. Wired into sclang's `initPrimitives()` (see integration/).
#[no_mangle]
pub extern "C" fn sc_rust_register_all() {
    registry::register_all();
}

#[cfg(test)]
mod test_host;
#[cfg(test)]
mod tests;
