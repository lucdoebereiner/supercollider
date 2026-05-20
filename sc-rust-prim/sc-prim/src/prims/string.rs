//! String in / string out. Demonstrates reading a `String` argument and
//! returning a freshly allocated `String`.

use crate::{sc_primitive_gc, Args, Gc, PrimError};

/// `RustPrim.reverseString(str)` -> the string reversed (by Unicode scalar).
pub fn reverse(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let s = args.arg(1).as_str()?;
    let reversed: String = s.chars().rev().collect();
    let result = gc.new_string(&reversed)?;
    args.set_result(result);
    Ok(())
}
sc_primitive_gc!(REVERSE, "_RustReverseString", 2, reverse);

/// `RustPrim.shout(str)` -> uppercased with a trailing '!'.
pub fn shout(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let s = args.arg(1).as_str()?;
    let result = gc.new_string(&format!("{}!", s.to_uppercase()))?;
    args.set_result(result);
    Ok(())
}
sc_primitive_gc!(SHOUT, "_RustShout", 2, shout);
