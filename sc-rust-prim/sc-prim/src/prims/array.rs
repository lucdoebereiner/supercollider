//! Primitives that build and return sclang objects. They take a `&Gc` scope;
//! allocation and write barriers are handled by the builder, so the bodies stay
//! free of `unsafe` and free of GC bookkeeping.

use crate::{sc_primitive_gc, Args, Gc, PrimError, Value};

/// `anInteger.rustPrimesUpTo` -> an `Array` of all primes <= n (receiver = n).
pub fn primes_up_to(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let n = args.arg(0).as_int()?.max(0) as usize;

    // Plain Rust: a sieve, no interpreter awareness at all.
    let mut sieve = vec![true; n + 1];
    let mut primes = Vec::new();
    let mut i = 2;
    while i <= n {
        if sieve[i] {
            primes.push(i as i32);
            let mut j = i * i;
            while j <= n {
                sieve[j] = false;
                j += i;
            }
        }
        i += 1;
    }

    // Hand the result back to sclang as an Array.
    let mut arr = gc.new_array(primes.len())?;
    for (idx, p) in primes.iter().enumerate() {
        arr.set(idx, Value::Int(*p));
    }
    args.set_result(arr.finish());
    Ok(())
}
sc_primitive_gc!(PRIMES_UP_TO, "_RustPrimesUpTo", 1, primes_up_to);

/// `anArray.rustHistogram(nbins)` -> an `Array` of bin counts.
/// Receiver is the data (any Array of numbers).
pub fn histogram(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let data = args.arg(0).as_f64_vec()?;
    let nbins = args.arg(1).as_int()?.max(1) as usize;

    let mut counts = vec![0i32; nbins];
    if !data.is_empty() {
        let min = data.iter().cloned().fold(f64::INFINITY, f64::min);
        let max = data.iter().cloned().fold(f64::NEG_INFINITY, f64::max);
        let span = (max - min).max(f64::MIN_POSITIVE);
        for &x in &data {
            let mut bin = (((x - min) / span) * nbins as f64) as usize;
            if bin >= nbins {
                bin = nbins - 1; // the max value lands in the last bin
            }
            counts[bin] += 1;
        }
    }

    let mut arr = gc.new_array(nbins)?;
    for (idx, c) in counts.iter().enumerate() {
        arr.set(idx, Value::Int(*c));
    }
    args.set_result(arr.finish());
    Ok(())
}
sc_primitive_gc!(HISTOGRAM, "_RustHistogram", 2, histogram);
