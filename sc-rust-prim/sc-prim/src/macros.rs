//! The glue that turns a safe Rust function into an `extern "C"` primitive:
//! it builds the [`Args`] view, catches panics so they never unwind into the
//! C++ interpreter, and maps the `Result` onto a VM error code.

use std::panic::{catch_unwind, AssertUnwindSafe};

use crate::args::Args;
use crate::error::PrimError;
use crate::gc::Gc;
use crate::host::errors::{ERR_FAILED, ERR_NONE};
use crate::host::{sc_post, sc_stack_ptr, ScVm};

fn finish(r: std::thread::Result<Result<(), PrimError>>) -> i32 {
    match r {
        Ok(Ok(())) => ERR_NONE,
        Ok(Err(e)) => e.code(),
        Err(_) => {
            unsafe { sc_post(c"sc-prim: a Rust primitive panicked (recovered)\n".as_ptr()) };
            ERR_FAILED
        }
    }
}

/// Run a value-only primitive (no allocation).
pub fn run_prim<F>(g: *mut ScVm, n: i32, f: F) -> i32
where
    F: FnOnce(&mut Args) -> Result<(), PrimError>,
{
    let r = catch_unwind(AssertUnwindSafe(|| {
        let sp = unsafe { sc_stack_ptr(g) };
        let mut args = unsafe { Args::new(g, sp, n) };
        f(&mut args)
    }));
    finish(r)
}

/// Run a primitive that may allocate objects, providing a [`Gc`] scope.
pub fn run_prim_gc<F>(g: *mut ScVm, n: i32, f: F) -> i32
where
    F: FnOnce(&mut Args, &Gc) -> Result<(), PrimError>,
{
    let r = catch_unwind(AssertUnwindSafe(|| {
        let sp = unsafe { sc_stack_ptr(g) };
        let mut args = unsafe { Args::new(g, sp, n) };
        let gc = unsafe { Gc::new(g) };
        // The result is written into the stack (a GC root) inside `f`, so it is
        // reachable before `gc` drops and re-enables collection.
        f(&mut args, &gc)
    }));
    finish(r)
}

/// Define a value-only primitive.
///
/// ```ignore
/// fn my_prim(args: &mut Args) -> Result<(), PrimError> { ... }
/// sc_primitive!(MY_PRIM, "_MyPrim", 2, my_prim);
/// ```
#[macro_export]
macro_rules! sc_primitive {
    ($desc:ident, $name:literal, $nargs:expr, $func:path) => {
        pub const $desc: $crate::PrimDesc = $crate::PrimDesc {
            name: concat!($name, "\0"),
            func: {
                extern "C" fn wrap(g: *mut $crate::host::ScVm, n: i32) -> i32 {
                    $crate::macros::run_prim(g, n, $func)
                }
                wrap
            },
            num_args: $nargs,
            var_args: 0,
        };
    };
}

/// Define a primitive that allocates objects (its function takes `&Gc`).
#[macro_export]
macro_rules! sc_primitive_gc {
    ($desc:ident, $name:literal, $nargs:expr, $func:path) => {
        pub const $desc: $crate::PrimDesc = $crate::PrimDesc {
            name: concat!($name, "\0"),
            func: {
                extern "C" fn wrap(g: *mut $crate::host::ScVm, n: i32) -> i32 {
                    $crate::macros::run_prim_gc(g, n, $func)
                }
                wrap
            },
            num_args: $nargs,
            var_args: 0,
        };
    };
}
