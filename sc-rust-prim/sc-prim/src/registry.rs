//! The list of primitives to register. This is the one place to add a new
//! primitive's descriptor once you've written it under `prims/`.

use crate::define;
use crate::prims::{array, foreign_demo, math, signal, string};

pub fn register_all() {
    // numbers (pure value functions, and one number -> Array)
    define(math::NTH_PRIME);
    define(math::HYPOT);
    define(math::FACTORIZE);

    // array / object builders
    define(array::PRIMES_UP_TO);
    define(array::HISTOGRAM);

    // signals (float arrays: create + process)
    define(signal::SINE);
    define(signal::NORMALIZE);
    define(signal::RMS);

    // strings
    define(string::REVERSE);
    define(string::SHOUT);

    // foreign objects (Rust value owned by an sclang object)
    define(foreign_demo::COUNTER_NEW);
    define(foreign_demo::COUNTER_NEXT);
    define(foreign_demo::COUNTER_FREE);

    // http (optional: requires the `http` Cargo feature)
    #[cfg(feature = "http")]
    define(crate::prims::http::HTTP_GET);
}
