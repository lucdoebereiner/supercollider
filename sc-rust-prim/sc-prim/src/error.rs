//! Error type returned by primitives. Wraps a sclang VM error code so it can be
//! handed straight back to the interpreter via the primitive's return value.

use crate::host::errors;

/// An error from a primitive, carrying a sclang VM error code.
///
/// Returning `Err(PrimError::WRONG_TYPE)` is the Rust equivalent of
/// `return errWrongType;` in a C++ primitive — the interpreter then runs the
/// method's fallback (`^this.primitiveFailed`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PrimError(i32);

impl PrimError {
    pub const FAILED: PrimError = PrimError(errors::ERR_FAILED);
    pub const WRONG_TYPE: PrimError = PrimError(errors::ERR_WRONG_TYPE);
    pub const INDEX_OUT_OF_RANGE: PrimError = PrimError(errors::ERR_INDEX_OUT_OF_RANGE);
    pub const OUT_OF_MEMORY: PrimError = PrimError(errors::ERR_OUT_OF_MEMORY);

    /// Wrap an arbitrary VM error code.
    pub fn new(code: i32) -> Self {
        PrimError(code)
    }

    /// The raw VM error code, suitable as a primitive return value.
    pub fn code(self) -> i32 {
        self.0
    }
}
