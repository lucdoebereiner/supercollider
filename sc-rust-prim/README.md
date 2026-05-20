# sc-prim — SuperCollider sclang primitives in safe Rust

A proof-of-concept abstraction layer for writing `sclang` **language primitives**
(the `_Foo` natives behind methods) in Rust instead of C++, so that the GC
discipline that makes C++ primitives error-prone is handled by the type system.

This tree lives **inside** a SuperCollider checkout (the `rust-primitives` branch),
under `sc-rust-prim/`. The sclang-side wiring is already applied on that branch;
see `../RUST_PRIMITIVES.md` at the repo root.

```
sc-rust-prim/
├── host/sc_host.h            # the thin C ABI everything agrees on
├── sc-prim/                  # the Rust crate (safe layer + example primitives)
│   ├── src/{host,slot,args,gc,foreign,macros,error}.rs        # the binding layer
│   └── src/prims/{math,array,signal,string,foreign_demo,http}.rs  # the example primitives
├── mock_host/                # standalone C++ host: runs the prims WITHOUT sclang
├── integration/              # the REAL backend + .sc glue + how-to-wire-in guide
├── TUTORIAL.md               # how to write your own primitive (+ GC explained)
└── build.sh                  # cargo test + build + run the demo
```

**New here? Read [`TUTORIAL.md`](TUTORIAL.md)** — a from-scratch guide with a
plain-language explanation of SuperCollider's garbage collector and a line-by-line
walkthrough of a real primitive.

## The example primitives

All live in [`sc-prim/src/prims/`](sc-prim/src/prims/). Open them — each is a
plain Rust function plus one `sc_primitive!`/`sc_primitive_gc!` line:

Each is hung on the natural receiver type as a `rust*` method (the prefix avoids
clashing with existing methods like `hypot`/`reverse`/`normalize`):

| category | sclang | source |
|---|---|---|
| numbers | `10.rustNthPrime`, `3.rustHypot(4)`, `360.rustFactorize` | [`prims/math.rs`](sc-prim/src/prims/math.rs) |
| arrays | `30.rustPrimesUpTo`, `data.rustHistogram(8)` | [`prims/array.rs`](sc-prim/src/prims/array.rs) |
| signals (float arrays) | `Signal.rustSine(n)`, `sig.rustNormalize`, `sig.rustRms` | [`prims/signal.rs`](sc-prim/src/prims/signal.rs) |
| strings | `"abc".rustReverse`, `"hi".rustShout` | [`prims/string.rs`](sc-prim/src/prims/string.rs) |
| foreign objects | `RustCounter("name")` (Rust value owned via `Drop` + finalizer) | [`prims/foreign_demo.rs`](sc-prim/src/prims/foreign_demo.rs) |
| http (opt-in feature) | `url.rustHttpGet` (pulls in the `ureq` crate) | [`prims/http.rs`](sc-prim/src/prims/http.rs) |

They are registered in [`sc-prim/src/registry.rs`](sc-prim/src/registry.rs) and
exposed to sclang by [`classes/RustExt.sc`](classes/RustExt.sc) (class extensions).

## Try it (no SuperCollider needed)

```sh
./build.sh
```

You should see all the primitives run against the mock host, including a Rust
object whose `Drop` fires both on explicit `.free` and at simulated GC time:

```
10.rustNthPrime  -> 29
30.rustPrimesUpTo -> [2, 3, 5, 7, 11, 13, 17, 19, 23, 29]
"hello".rustReverse -> "olleh"
  [Rust Drop] Counter 'c1' freed at count 2
  [Rust Drop] Counter 'c2' freed at count 1
```

`cargo test` (run by `build.sh`) drives the same primitives through an in-process
backend and checks results, including the panic-to-error-code path.

## The idea

The hard part of an sclang primitive is not C++ memory management — it is
sclang's incremental tri-color GC: **write barriers** and keeping freshly
allocated objects **alive across allocations**. Switching language doesn't fix
that; a typed wrapper does. Here:

| Concern | How it's handled |
|---|---|
| Read args / return scalars | `Args` + `Value`; tag-checked, `?`-propagating errors |
| Allocate objects | `Gc` RAII scope = sclang "delayed collection"; no mid-build collection |
| Write barriers | applied *inside* `ArrayBuilder::set` — impossible to forget |
| Foreign Rust objects | `foreign::attach::<T>` + finalizer ⇒ Rust `Drop` at GC time; `take` for eager free |
| Panics | caught at the FFI boundary, turned into `errFailed` |

So a primitive reads like ordinary Rust:

```rust
fn primes_up_to(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let n = args.arg(1).as_int()?.max(0) as usize;
    let primes = sieve(n);                 // plain Rust, zero GC awareness
    let mut arr = gc.new_array(primes.len())?;
    for (i, p) in primes.iter().enumerate() {
        arr.set(i, Value::Int(*p));        // barrier handled for you
    }
    args.set_result(arr.finish());
    Ok(())
}
sc_primitive_gc!(PRIMES_UP_TO, "_RustPrimesUpTo", 2, primes_up_to);
```

## Architecture: one ABI, two backends

The Rust crate depends only on `host/sc_host.h`. That ABI has two
implementations:

- **`mock_host/mock_host.cpp`** — malloc-backed, lets the whole thing
  build and run on its own (what `build.sh` uses).
- **`integration/sc_rust_shim.cpp`** — the production backend, wired to sclang's
  real `PyrSlot`/`PyrObject`/GC. It is the *only* file that includes SC headers,
  so SC version drift is contained to one place.

To run inside a real `sclang`, follow **`integration/README.md`** (these edits
are already applied on the `rust-primitives` branch; see `../RUST_PRIMITIVES.md`).
With `SC_RUST_PRIMITIVES=ON` the build also installs `classes/RustExt.sc` into the
Extensions dir.

## Status & limits

- Verified: value primitives, array/string builders, foreign objects with
  `Drop`, panic recovery — all green under `cargo test` and the mock demo.
- Not done here: actually patching the SuperCollider tree (left to you), a
  runtime plugin loader (sclang links primitives statically), and broader type
  coverage (symbols, FloatArray/Int8Array data, keyword args).
- The residual cases the abstraction *can't* hide: holding an sclang object
  reference on the Rust side past the call, and primitives that re-enter the
  interpreter. Both would need explicit rooting.
