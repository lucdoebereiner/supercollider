//! Pure value-in/value-out primitives. These never allocate and never touch the
//! GC — the abstraction makes them read like ordinary Rust.

use crate::{sc_primitive, Args, PrimError, Value};

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

/// `RustPrim.nthPrime(n)` -> the nth prime, or `nil` for n < 1.
pub fn nth_prime(args: &mut Args) -> Result<(), PrimError> {
    let n = args.arg(1).as_int()? as i64;
    let result = match nth_prime_value(n) {
        Some(p) => Value::Int(p as i32),
        None => Value::Nil,
    };
    args.set_result(result);
    Ok(())
}
sc_primitive!(NTH_PRIME, "_RustNthPrime", 2, nth_prime);

/// `RustPrim.hypot(a, b)` -> sqrt(a^2 + b^2). Accepts ints or floats.
pub fn hypot(args: &mut Args) -> Result<(), PrimError> {
    let a = args.arg(1).as_float()?;
    let b = args.arg(2).as_float()?;
    args.set_result(Value::Float(a.hypot(b)));
    Ok(())
}
sc_primitive!(HYPOT, "_RustHypot", 3, hypot);

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
