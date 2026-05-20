//! Example primitives, grouped the way sclang groups its own (math, array, …).
//! Each is a plain Rust function plus one `sc_primitive!` / `sc_primitive_gc!`
//! line. Add new ones here and list them in [`crate::registry`].

pub mod array;
pub mod foreign_demo;
#[cfg(feature = "http")]
pub mod http;
pub mod math;
pub mod signal;
pub mod string;
