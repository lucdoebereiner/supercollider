//! Number primitives. The first two are pure value-in/value-out (no allocation,
//! no GC). `factorize` returns an Array, so it takes a `&Gc` scope.

use crate::{sc_primitive, sc_primitive_gc, Args, Gc, PrimError, Value};

fn is_prime(n: i64) -> bool {
    if n < 2 {
        return false;
    }
    let mut d = 2;
    while d * d <= n {
        if n % d == 0 {
            return false;
        }
        d += 1;
    }
    true
}

/// 1-based nth prime: nth_prime(1) == 2, nth_prime(3) == 5.
fn nth_prime_value(n: i64) -> Option<i64> {
    if n < 1 {
        return None;
    }
    let mut count = 0;
    let mut candidate = 1;
    loop {
        candidate += 1;
        if is_prime(candidate) {
            count += 1;
            if count == n {
                return Some(candidate);
            }
        }
    }
}

/// `anInteger.rustNthPrime` -> the nth prime, or `nil` for n < 1.
/// Receiver is the index (instance method on Integer).
pub fn nth_prime(args: &mut Args) -> Result<(), PrimError> {
    let n = args.arg(0).as_int()? as i64;
    let result = match nth_prime_value(n) {
        Some(p) => Value::Int(p as i32),
        None => Value::Nil,
    };
    args.set_result(result);
    Ok(())
}
sc_primitive!(NTH_PRIME, "_RustNthPrime", 1, nth_prime);

/// `a.rustHypot(b)` -> sqrt(a^2 + b^2). Accepts ints or floats.
/// Receiver is `a` (instance method on SimpleNumber).
pub fn hypot(args: &mut Args) -> Result<(), PrimError> {
    let a = args.arg(0).as_float()?;
    let b = args.arg(1).as_float()?;
    args.set_result(Value::Float(a.hypot(b)));
    Ok(())
}
sc_primitive!(HYPOT, "_RustHypot", 2, hypot);

/// `anInteger.rustFactorize` -> an Array of n's prime factors (with multiplicity).
/// A "number in, Array out" example; receiver is the number.
pub fn factorize(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let mut n = args.arg(0).as_int()? as i64;
    let mut factors = Vec::new();
    let mut d = 2;
    while d * d <= n {
        while n % d == 0 {
            factors.push(d as i32);
            n /= d;
        }
        d += 1;
    }
    if n > 1 {
        factors.push(n as i32);
    }

    let mut arr = gc.new_array(factors.len())?;
    for (i, f) in factors.iter().enumerate() {
        arr.set(i, Value::Int(*f));
    }
    args.set_result(arr.finish());
    Ok(())
}
sc_primitive_gc!(FACTORIZE, "_RustFactorize", 1, factorize);

#[cfg(test)]
mod unit {
    use super::*;

    #[test]
    fn primes() {
        assert_eq!(nth_prime_value(1), Some(2));
        assert_eq!(nth_prime_value(3), Some(5));
        assert_eq!(nth_prime_value(10), Some(29));
        assert_eq!(nth_prime_value(0), None);
    }
}
