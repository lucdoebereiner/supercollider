//! String in / string out. Demonstrates reading a `String` argument and
//! returning a freshly allocated `String`.

use crate::{sc_primitive_gc, Args, Gc, PrimError};

/// `aString.rustReverse` -> the string reversed (by Unicode scalar). Receiver = the string.
pub fn reverse(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let s = args.arg(0).as_str()?;
    let reversed: String = s.chars().rev().collect();
    let result = gc.new_string(&reversed)?;
    args.set_result(result);
    Ok(())
}
sc_primitive_gc!(REVERSE, "_RustReverseString", 1, reverse);

/// `aString.rustShout` -> uppercased with a trailing '!'. Receiver = the string.
pub fn shout(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let s = args.arg(0).as_str()?;
    let result = gc.new_string(&format!("{}!", s.to_uppercase()))?;
    args.set_result(result);
    Ok(())
}
sc_primitive_gc!(SHOUT, "_RustShout", 1, shout);
