//! The list of primitives to register. This is the one place to add a new
//! primitive's descriptor once you've written it under `prims/`.

use crate::define;
use crate::prims::{array, foreign_demo, math, string};

pub fn register_all() {
    // math (pure value functions)
    define(math::NTH_PRIME);
    define(math::HYPOT);

    // array / object builders
    define(array::PRIMES_UP_TO);
    define(array::HISTOGRAM);

    // strings
    define(string::REVERSE);
    define(string::SHOUT);

    // foreign objects (Rust value owned by an sclang object)
    define(foreign_demo::COUNTER_NEW);
    define(foreign_demo::COUNTER_NEXT);
    define(foreign_demo::COUNTER_FREE);
}
