//! HTTP GET primitives — a showcase of pulling a real Rust crate (`ureq`) into
//! sclang. Doing this in a C++ primitive would mean hand-rolling sockets or
//! linking curl; in Rust it's a few lines.
//!
//! Gated behind the `http` Cargo feature so the core crate stays dependency-free.
//! Build with:  `cargo build --release --features http`
//!
//! Two flavours:
//!   - `rustHttpGet` (blocking) — simple, but freezes the language thread until
//!     the request returns. Fine for scripting.
//!   - `RustHttpRequest` (non-blocking) — starts the request on a background
//!     thread and returns immediately; sclang polls `isReady`/`result`/`error`.
//!     This is the realistic shape, because sclang is single-threaded: the worker
//!     thread touches only Rust memory (a `Pending`), and the *language* thread is
//!     the only one that ever allocates the result String.

use std::time::Duration;

use crate::async_value::Pending;
use crate::foreign;
use crate::{sc_primitive, sc_primitive_gc, Args, Gc, PrimError, Value};

/// Result of a fetch: the body on success, or an error message.
type HttpResult = Result<String, String>;

/// The actual (blocking) network call. Runs on whichever thread invokes it.
fn fetch(url: &str) -> HttpResult {
    match ureq::get(url).timeout(Duration::from_secs(30)).call() {
        Ok(resp) => resp.into_string().map_err(|e| e.to_string()),
        Err(e) => Err(e.to_string()),
    }
}

// ---- blocking ------------------------------------------------------------

/// `aString.rustHttpGet` -> the response body as a String, or `nil` on error.
/// Receiver is the URL string. Blocks the language thread.
pub fn http_get(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let url = args.arg(0).as_str()?;
    match fetch(url) {
        Ok(body) => args.set_result(gc.new_string(&body)?),
        Err(_) => args.set_result(Value::Nil),
    }
    Ok(())
}
sc_primitive_gc!(HTTP_GET, "_RustHttpGet", 1, http_get);

// ---- non-blocking --------------------------------------------------------
//
// The foreign object owns a `Pending<HttpResult>`. Starting it spawns a thread
// that runs `fetch` and stores the result in the Pending (Rust memory only). The
// language thread polls and, when ready, allocates the SC String. The worker
// never touches the interpreter, so no rooting or locking of the VM is needed.

/// `RustHttpRequest(url)` body: spawn the request, attach the `Pending` to self.
pub fn http_start(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let url = args.arg(1).as_str()?.to_owned(); // own it: it moves to the thread
    let pending: Pending<HttpResult> = Pending::spawn(move || fetch(&url));
    unsafe { foreign::attach(args.vm(), obj, pending) };
    args.set_result(Value::Obj(obj)); // return self
    Ok(())
}
sc_primitive!(HTTP_START, "_RustHttpStart", 2, http_start);

/// `aRustHttpRequest.isReady` -> Boolean: has the request finished?
pub fn http_is_ready(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let ready = unsafe { foreign::with_mut::<Pending<HttpResult>, _>(obj, |p| p.is_ready()) }
        .unwrap_or(false);
    args.set_result(Value::Bool(ready));
    Ok(())
}
sc_primitive!(HTTP_IS_READY, "_RustHttpIsReady", 1, http_is_ready);

/// `aRustHttpRequest.result` -> the body String, or `nil` (pending or errored).
pub fn http_result(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let body = unsafe {
        foreign::with_mut::<Pending<HttpResult>, _>(obj, |p| {
            p.with(|v| match v {
                Some(Ok(body)) => Some(body.clone()),
                _ => None,
            })
        })
    }
    .flatten();
    match body {
        Some(body) => args.set_result(gc.new_string(&body)?),
        None => args.set_result(Value::Nil),
    }
    Ok(())
}
sc_primitive_gc!(HTTP_RESULT, "_RustHttpResult", 1, http_result);

/// `aRustHttpRequest.error` -> the error message String, or `nil`.
pub fn http_error(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let err = unsafe {
        foreign::with_mut::<Pending<HttpResult>, _>(obj, |p| {
            p.with(|v| match v {
                Some(Err(msg)) => Some(msg.clone()),
                _ => None,
            })
        })
    }
    .flatten();
    match err {
        Some(msg) => args.set_result(gc.new_string(&msg)?),
        None => args.set_result(Value::Nil),
    }
    Ok(())
}
sc_primitive_gc!(HTTP_ERROR, "_RustHttpError", 1, http_error);
