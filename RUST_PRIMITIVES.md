# Writing sclang primitives in Rust — integration changes

This branch (`rust-primitives`, based on `develop` @ `462f0f1aa`) adds an
**opt-in** path for implementing sclang *language primitives* (the `_Foo`
natives behind methods) in Rust instead of C++, so that SuperCollider's GC
discipline (write barriers, keeping freshly allocated objects alive across
allocations) is handled by a typed Rust layer rather than by hand.

The Rust code itself lives **in this repository** under `sc-rust-prim/`
(a self-contained Cargo project). The rest of this repo is stock SuperCollider
plus the minimal C++/CMake glue needed to link it in. With the feature disabled
(the default), this branch is behaviorally identical to upstream `develop`.

---

## What changed in this repo

Four changes, all gated behind a new CMake option `SC_RUST_PRIMITIVES` (default
`OFF`). When `OFF`, none of the new code is compiled or called.

### 1. New file — `lang/LangPrimSource/sc_rust_shim.cpp`

The C-ABI shim: the only file that includes SuperCollider headers. It implements
the abstract `sc_host.h` interface (defined in `sc-rust-prim/host`) in terms
of sclang internals — stack access, object allocation (`newPyrArray`,
`newPyrStringN`), the write barrier (`PyrGC::GCWrite`), delayed-collection
scopes, and finalizer installation (`InstallFinalizer`). It also forwards
primitive registration to `definePrimitive`. Compiled only when
`SC_RUST_PRIMITIVES=ON`.

### 2. New file — `lang/LangPrimSource/RustPrim.sc` (reference class library)

The sclang classes (`RustPrim`, `RustCounter`) whose methods call the Rust
primitives. This file is **not** auto-compiled (kept out of `SCClassLibrary` so
default builds emit no "primitive not found" warnings). To use it, copy it into
your user extensions directory — see *Run* below.

### 3. `lang/CMakeLists.txt` — option + link

Two blocks added:

```cmake
# before add_library(libsclang ...)
option(SC_RUST_PRIMITIVES "Build language primitives written in Rust (sc-prim)" OFF)
if(SC_RUST_PRIMITIVES)
    set(SC_RUST_PRIM_DIR "${CMAKE_SOURCE_DIR}/sc-rust-prim" CACHE PATH "...")
    list(APPEND sclang_sources LangPrimSource/sc_rust_shim.cpp)
endif()
```

```cmake
# after target_link_libraries(libsclang tlsf ...)
if(SC_RUST_PRIMITIVES)
    set(SC_RUST_PRIM_LIB "${SC_RUST_PRIM_DIR}/sc-prim/target/release/libsc_prim.a" CACHE FILEPATH "...")
    # (errors early if the .a is missing)
    target_include_directories(libsclang PRIVATE "${SC_RUST_PRIM_DIR}/host")
    target_compile_definitions(libsclang PRIVATE SC_USE_RUST_PRIMITIVES)
    target_link_libraries(libsclang "${SC_RUST_PRIM_LIB}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(libsclang pthread dl m)   # Rust staticlib deps
    endif()
endif()
```

### 4. `lang/LangPrimSource/PyrPrimitive.cpp` — register at startup

In `initPrimitives()`, just before the final bookkeeping:

```cpp
#ifdef SC_USE_RUST_PRIMITIVES
    extern "C" void sc_rust_register_all();   // from libsc_prim.a
    sc_rust_register_all();
#endif
```

---

## Build

```sh
# (run from the repo root)
# 1. Build the Rust static library (one-time / on Rust changes)
( cd sc-rust-prim/sc-prim && cargo build --release )   # -> target/release/libsc_prim.a

# 2. Configure SuperCollider with the feature ON
cmake -B build -DSC_RUST_PRIMITIVES=ON      # add your usual SC options
#   (SC_RUST_PRIM_DIR defaults to ./sc-rust-prim; override only if you move it)
cmake --build build --target sclang -j
```

`cmake` prints `sclang: Rust primitives ENABLED (...)` and fails fast if the
`.a` is missing.

## Run

```sh
# make the classes available
cp sc-rust-prim/integration/SCRustPrim.sc "$HOME/.local/share/SuperCollider/Extensions/"
# (or wherever Platform.userExtensionDir points)
```

```supercollider
RustPrim.nthPrime(10);            // 29       (number -> number)
RustPrim.factorize(360);          // [2,2,2,3,3,5]  (number -> Array)
RustPrim.primesUpTo(30);          // [ 2, 3, 5, 7, 11, 13, 17, 19, 23, 29 ]
RustPrim.sineSignal(512).plot;    // one cycle of a sine, as a Signal
Signal.sineFill(64, [1]).rustNormalize.rustRms;   // process a Signal
RustPrim.reverseString("hello");  // "olleh"

c = RustCounter("voices");
c.next; c.next; c.next;           // 1, 2, 3
c.free;                           // [Rust Drop] Counter 'voices' freed at count 3
```

The HTTP example needs the optional feature
(`cargo build --release --features http`), then `RustPrim.httpGet("http://example.com")`.

---

## Want to write your own primitive?

See **`sc-rust-prim/TUTORIAL.md`** — a from-scratch guide including a plain-language
explanation of SuperCollider's garbage collector and the two rules a primitive
must follow.

## How the Rust side stays safe (summary)

Full detail in `sc-rust-prim/README.md` and `sc-rust-prim/TUTORIAL.md`. In short,
a primitive is an ordinary Rust function:

```rust
fn primes_up_to(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let n = args.arg(1).as_int()?.max(0) as usize;
    let primes = sieve(n);                 // plain Rust
    let mut arr = gc.new_array(primes.len())?;
    for (i, p) in primes.iter().enumerate() {
        arr.set(i, Value::Int(*p));        // write barrier applied internally
    }
    args.set_result(arr.finish());
    Ok(())
}
sc_primitive_gc!(PRIMES_UP_TO, "_RustPrimesUpTo", 2, primes_up_to);
```

- `Gc` is an RAII handle for sclang's *delayed collection* context — no
  collection runs while objects are being built.
- `ArrayBuilder::set` applies the GC write barrier, so it can't be forgotten.
- Foreign Rust values are owned via `foreign::attach::<T>` + a finalizer, so
  Rust's `Drop` runs at GC time (or eagerly via an explicit `.free`).
- Panics are caught at the FFI boundary and converted to `errFailed`.

## Design notes for reviewers / caveats

- **Opt-in & neutral.** Default builds (`SC_RUST_PRIMITIVES=OFF`) are unchanged.
- **One ABI, isolated.** Rust depends only on the C ABI in `sc-rust-prim/host/sc_host.h`;
  `sc_rust_shim.cpp` is the single place that tracks sclang's struct layout, so
  version drift is contained there.
- **Static link, not a plugin.** sclang links primitives statically; adding one
  means rebuilding `libsclang`. There is no runtime language-primitive loader.
- **Foreign objects** reserve instance-var slots 0 (pointer) and 1 (finalizer);
  declare those first in any class using `foreign::attach`.
- **Not abstracted away:** holding an sclang object reference on the Rust side
  past a primitive call, and primitives that re-enter the interpreter — both
  would need explicit GC rooting.
- **Status:** the Rust crate's `cargo test` (19 tests — every example plus three
  memory-safety tests: no-leak-on-free, no-leak-on-finalizer, no-double-free) and
  a standalone mock host pass; `sc_rust_shim.cpp` syntax-checks against these SC
  headers; the `http` feature compiles. A full end-to-end build inside sclang has
  not yet been run on this machine.
