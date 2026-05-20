# Wiring the Rust primitives into sclang

> **Note:** on the `rust-primitives` branch these edits are **already applied**
> (opt-in via the `SC_RUST_PRIMITIVES` CMake option). See `../../RUST_PRIMITIVES.md`
> at the repo root for the as-built description and quick start. This file is
> kept as a reference explaining *what* the wiring does and how to reproduce it
> in a clean tree.

To make the primitives callable from `sclang`, three edits to the SC source are
required. The Rust side is decoupled from sclang's internals by the `sc_host.h`
C ABI; `sc_rust_shim.cpp` is the only file that includes SC headers.

## 1. Build the Rust static library

```sh
cd ../sc-prim
cargo build --release
# produces ../sc-prim/target/release/libsc_prim.a
```

## 2. Add the shim + static lib to the lang build

In `SuperCollider/lang/CMakeLists.txt`, add the shim to `sclang_sources`
(the list around line 32):

```cmake
set(sclang_sources
    # ... existing entries ...
    LangPrimSource/sc_rust_shim.cpp   # <-- copy/symlink integration/sc_rust_shim.cpp here
)
```

and link the Rust archive after `add_library(libsclang STATIC ...)` (line ~200):

```cmake
target_link_libraries(libsclang
    /absolute/path/to/sc-rust-prim/sc-prim/target/release/libsc_prim.a)
# Rust staticlib needs these on Linux:
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(libsclang pthread dl m)
endif()
```

Make `sc_host.h` reachable from the shim, e.g.:

```cmake
target_include_directories(libsclang PRIVATE /absolute/path/to/sc-rust-prim/host)
```

> Copy `sc_rust_shim.cpp` into `lang/LangPrimSource/` (or add its directory to
> the target's include/source paths). It must be compiled where the SC headers
> `GC.h`, `PyrKernel.h`, `PyrObject.h`, `PyrPrimitive.h`, `VMGlobals.h` resolve.

## 3. Register the primitives at startup

In `SuperCollider/lang/LangPrimSource/PyrPrimitive.cpp`, inside `initPrimitives()`
(near the other `initXxxPrimitives()` calls, ~line 4100), add:

```cpp
extern "C" void sc_rust_register_all();   // from libsc_prim.a
sc_rust_register_all();
```

## 4. Install the class library

Copy `SCRustPrim.sc` into your user extensions directory:

```sh
cp SCRustPrim.sc "$(sclang -d ... )"   # or just into Platform.userExtensionDir
```

Rebuild sclang, recompile the class library, and:

```supercollider
RustPrim.nthPrime(10);     // 29
RustPrim.primesUpTo(30);   // [ 2, 3, 5, 7, 11, 13, 17, 19, 23, 29 ]
RustPrim.reverseString("hello"); // "olleh"

c = RustCounter("voices");
c.next; c.next; c.next;    // 1, 2, 3
c.free;                    // [Rust Drop] Counter 'voices' freed at count 3
```

## Notes / caveats

- **ABI version coupling.** `sc_rust_shim.cpp` hard-codes SC's struct/function
  names. If you bump SuperCollider, recompile the shim against the new headers
  (and adjust if a field was renamed). The Rust crate itself does not change.
- **Static link, not a plugin.** Adding/removing a primitive means rebuilding
  `libsclang`. There is no runtime primitive loader for the language.
- **Foreign objects** reserve instance-var slots 0 (pointer) and 1 (finalizer).
  Keep them as the first two `var`s in any class that uses `foreign::attach`.
