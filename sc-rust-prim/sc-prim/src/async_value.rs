//! A value computed on a background thread and polled from the language thread.
//!
//! sclang is single-threaded: you may only touch the interpreter (allocate
//! objects, call methods, run the GC) from the language thread. So the safe shape
//! for async work in a primitive is:
//!
//!   - the heavy work runs on a Rust thread that touches ONLY Rust memory, and
//!   - sclang *polls* for the result (typically from a Routine on a clock).
//!
//! Nothing here calls back into the interpreter off-thread, which is what makes it
//! safe. (The alternative — pushing the result by invoking an sclang function from
//! the worker thread — would require running on the language thread via SC's
//! scheduler/lang-lock, and is much easier to get wrong.)

use std::sync::{Arc, Mutex};

/// A slot a background thread fills in exactly once. The handle is cheap to hold
/// inside a foreign object; the worker keeps its own clone of the shared state.
pub struct Pending<T> {
    state: Arc<Mutex<Option<T>>>,
}

impl<T: Send + 'static> Pending<T> {
    /// Run `work` on a new detached background thread. The `Pending` becomes
    /// ready when it finishes. If the `Pending` is dropped first, the worker
    /// still completes and drops its own clone of the state — no leak, no block.
    pub fn spawn(work: impl FnOnce() -> T + Send + 'static) -> Self {
        let state = Arc::new(Mutex::new(None));
        let worker = Arc::clone(&state);
        std::thread::spawn(move || {
            let value = work();
            *lock(&worker) = Some(value);
        });
        Pending { state }
    }

    /// True once the background work has finished.
    pub fn is_ready(&self) -> bool {
        self.with(|v| v.is_some())
    }

    /// Inspect the result without consuming it (so `result`/`error`/`isReady`
    /// can each be called any number of times).
    pub fn with<R>(&self, f: impl FnOnce(Option<&T>) -> R) -> R {
        f(lock(&self.state).as_ref())
    }

    /// Take the result if ready, leaving the slot empty. (Used by tests; the HTTP
    /// primitives use [`Pending::with`] so the value stays readable.)
    pub fn take(&self) -> Option<T> {
        lock(&self.state).take()
    }
}

/// Lock, recovering from a poisoned mutex (a panicked worker) rather than
/// propagating the panic onto the language thread.
fn lock<T>(m: &Mutex<T>) -> std::sync::MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn resolves_on_background_thread() {
        let p = Pending::spawn(|| {
            std::thread::sleep(Duration::from_millis(10));
            21 * 2
        });
        // poll the way an sclang Routine would
        let mut spins = 0;
        while !p.is_ready() {
            std::thread::sleep(Duration::from_millis(1));
            spins += 1;
            assert!(spins < 5000, "background work never finished");
        }
        assert_eq!(p.with(|v| v.copied()), Some(42)); // readable without consuming
        assert_eq!(p.take(), Some(42)); // ...then taken once
        assert_eq!(p.take(), None);
    }
}
