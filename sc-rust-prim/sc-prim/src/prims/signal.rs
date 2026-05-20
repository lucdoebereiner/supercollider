//! Primitives that read and write `Signal`s — sclang's float arrays, used for
//! wavetables, envelopes, FFT buffers, etc. Reading is a `&[f32]` view of the
//! caller's Signal; writing goes into a freshly allocated Signal.

use std::f32::consts::TAU;

use crate::{sc_primitive_gc, Args, Gc, PrimError};

/// `RustPrim.sineSignal(size)` -> a Signal holding one cycle of a sine wave.
/// A minimal "create a Signal" example (a wavetable generator).
pub fn sine(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let n = args.arg(1).as_int()?.max(0) as usize;
    let mut sig = gc.new_signal(n)?;
    let buf = sig.as_mut_slice();
    for (i, sample) in buf.iter_mut().enumerate() {
        *sample = (TAU * i as f32 / n as f32).sin();
    }
    args.set_result(sig.finish());
    Ok(())
}
sc_primitive_gc!(SINE, "_RustSineSignal", 2, sine);

/// `aSignal.rustNormalize` -> a NEW Signal scaled so its peak magnitude is 1.0
/// (silence is returned unchanged). A "process a Signal" example: read input
/// samples, compute, write a fresh output Signal.
pub fn normalize(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let input = args.arg(0).as_f32_slice()?; // receiver is the Signal
    let peak = input.iter().fold(0.0f32, |m, &x| m.max(x.abs()));
    let scale = if peak > 0.0 { 1.0 / peak } else { 1.0 };

    let mut out = gc.new_signal(input.len())?;
    let buf = out.as_mut_slice();
    for (dst, &src) in buf.iter_mut().zip(input) {
        *dst = src * scale;
    }
    args.set_result(out.finish());
    Ok(())
}
sc_primitive_gc!(NORMALIZE, "_RustNormalizeSignal", 1, normalize);

/// `aSignal.rustRms` -> the root-mean-square level as a Float.
/// A "process a Signal, return a number" example.
pub fn rms(args: &mut Args, _gc: &Gc) -> Result<(), PrimError> {
    let input = args.arg(0).as_f32_slice()?;
    let value = if input.is_empty() {
        0.0
    } else {
        let sum_sq: f64 = input.iter().map(|&x| (x as f64) * (x as f64)).sum();
        (sum_sq / input.len() as f64).sqrt()
    };
    args.set_result(crate::Value::Float(value));
    Ok(())
}
sc_primitive_gc!(RMS, "_RustSignalRms", 1, rms);
