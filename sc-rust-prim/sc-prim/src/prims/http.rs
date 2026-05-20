//! An HTTP GET primitive — a showcase of pulling a real Rust crate (`ureq`)
//! into sclang. Doing this in a C++ primitive would mean hand-rolling sockets
//! or linking curl; in Rust it's a few lines.
//!
//! Gated behind the `http` Cargo feature so the core crate stays
//! dependency-free. Build with:  `cargo build --release --features http`
//!
//! CAVEAT: this is a *blocking* request. It runs on the language thread and
//! will freeze the interpreter until it returns or times out. That is fine for
//! a demo / scripting, but for real use you would run the request on a
//! background thread and deliver the result asynchronously (e.g. via a
//! registered callback). See TUTORIAL.md for the threading discussion.

use crate::{sc_primitive_gc, Args, Gc, PrimError, Value};

/// `RustPrim.httpGet(url)` -> the response body as a String, or `nil` on error.
pub fn http_get(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let url = args.arg(1).as_str()?;
    let result = ureq::get(url)
        .timeout(std::time::Duration::from_secs(10))
        .call()
        .ok()
        .and_then(|resp| resp.into_string().ok());

    match result {
        Some(body) => args.set_result(gc.new_string(&body)?),
        None => args.set_result(Value::Nil),
    }
    Ok(())
}
sc_primitive_gc!(HTTP_GET, "_RustHttpGet", 2, http_get);
