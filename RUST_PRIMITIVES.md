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

### 2. Class extensions — `sc-rust-prim/classes/RustExt.sc`

The sclang glue. Rather than a wrapper class, the example primitives are hung on
the natural receiver types as `rust*` methods — `+ Integer { rustNthPrime ... }`,
`+ String { rustReverse ... }`, `+ Signal { ... }`, etc. — plus a `RustCounter`
class for the foreign-object example. The `rust` prefix avoids clashing with
existing methods (`hypot`, `reverse`, `normalize` already exist).

This file lives in the self-contained `sc-rust-prim/` project and is kept out of
`SCClassLibrary` (so non-feature builds emit no "primitive not found" warnings).
When `SC_RUST_PRIMITIVES=ON`, the build **installs** it to
`share/SuperCollider/Extensions` (step 3); for run-from-build-tree use, copy it
into your user extensions dir.

### 3. `lang/CMakeLists.txt` — option + link + install

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

On **Linux**, `cmake --install` puts `RustExt.sc` in the system extension dir for
you (`<prefix>/share/SuperCollider/Extensions`, which that sclang scans). On
**macOS/Windows** the system extension dir is an absolute OS path outside the
install prefix, so the build can't place it there — copy it by hand. Either way,
running from a build tree also means copying it once:

```sh
# Linux build tree, or any platform: drop it where sclang looks
cp sc-rust-prim/classes/RustExt.sc "$HOME/.local/share/SuperCollider/Extensions/"
# macOS: ~/Library/Application Support/SuperCollider/Extensions/
# (i.e. Platform.userExtensionDir — check with `Platform.userExtensionDir` in sclang)
```

The example primitives are `rust*` methods on the natural types:

```supercollider
10.rustNthPrime;                  // 29       (number -> number)
360.rustFactorize;                // [2,2,2,3,3,5]  (number -> Array)
30.rustPrimesUpTo;                // [ 2, 3, 5, 7, 11, 13, 17, 19, 23, 29 ]
3.rustHypot(4);                   // 5.0
[0, 0.1, 0.9, 1].rustHistogram(2);
Signal.rustSine(512).plot;        // create a Signal (class method)
Signal.sineFill(64, [1]).rustNormalize.rustRms;   // process a Signal
"hello".rustReverse;              // "olleh"

c = RustCounter("voices");
c.next; c.next; c.next;           // 1, 2, 3
c.free;                           // [Rust Drop] Counter 'voices' freed at count 3
```

The HTTP example needs the optional feature
(`cargo build --release --features http`): blocking `"http://example.com".rustHttpGet`,
or non-blocking `RustHttpRequest("http://example.com").onComplete { |body, err| ... }`
(runs on a background thread, polled — never freezes the language thread).

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
  past a primitive call, and primitives that re-enter the interpreter — both need
  explicit rooting. SC has no root-handle API, so the fix is to store the
  reference in an object slot (`object::set_field`) where the collector can reach
  it; the crate provides that helper and documents it in `sc-rust-prim/TUTORIAL.md`.
- **Status:** the Rust crate's `cargo test` (19 tests — every example plus three
  memory-safety tests: no-leak-on-free, no-leak-on-finalizer, no-double-free) and
  a standalone mock host pass; `sc_rust_shim.cpp` syntax-checks against these SC
  headers; the `http` feature compiles. A full end-to-end build inside sclang has
  not yet been run on this machine.
