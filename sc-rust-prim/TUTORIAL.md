# Writing SuperCollider primitives in Rust — a tutorial

This walks you through writing sclang **primitives** (the native `_Foo` functions
behind methods) in Rust, using the `sc-prim` crate. No prior knowledge of
SuperCollider's internals is assumed.

- [What a primitive is](#what-a-primitive-is)
- [How the garbage collector works (plain language)](#how-the-garbage-collector-works-plain-language)
- [Anatomy of a primitive](#anatomy-of-a-primitive)
- [Walkthrough: add your own primitive](#walkthrough-add-your-own-primitive)
- [Reading arguments and returning values](#reading-arguments-and-returning-values)
- [Allocating objects: arrays, strings, signals](#allocating-objects-arrays-strings-signals)
- [Owning a Rust value from sclang (foreign objects)](#owning-a-rust-value-from-sclang-foreign-objects)
- [Errors and panics](#errors-and-panics)
- [Example: an HTTP request](#example-an-http-request)
- [Testing and checking for leaks](#testing-and-checking-for-leaks)

---

## What a primitive is

In sclang, most methods are written in SuperCollider itself, but the lowest-level
ones call into native code. A method body that begins with `_SomeName` invokes a
**primitive** — a native function registered under that name:

```supercollider
+ Integer {
    rustNthPrime { _RustNthPrime; ^this.primitiveFailed }
}
```

When you call `10.rustNthPrime`, the interpreter pushes the receiver (`10`) onto
a stack and calls the native function registered as `_RustNthPrime`. If that
function signals an error, the rest of the method runs — here
`^this.primitiveFailed`, the conventional fallback.

A primitive acts on its **receiver** (`self`) plus any arguments, so the natural
design is to hang each method on the type it operates on (`+ Integer`, `+ String`,
`+ Signal`, …) — exactly how SC's own primitives work (`abs { _Abs; … }` on
`SimpleNumber`). The examples here use a `rust` prefix so they don't clash with
existing methods (`hypot`, `reverse`, `normalize` all already exist).

There are three kinds of primitive you will write, in increasing order of GC
involvement:

1. **Value functions** — take numbers/strings, return a number/bool. No
   allocation, no GC. (e.g. `rustNthPrime`, `rustHypot`)
2. **Object builders** — construct and return an Array, String, or Signal.
   These touch the GC, but the crate handles it. (e.g. `rustPrimesUpTo`, `rustSine`)
3. **Foreign-object primitives** — attach a long-lived Rust value to an sclang
   object. These use finalizers. (e.g. `RustCounter`)

---

## How the garbage collector works (plain language)

You **never call `free`** on sclang objects (Arrays, Strings, Signals, class
instances). SuperCollider has a **garbage collector (GC)**: a bookkeeper that
automatically reclaims an object once nothing can reach it anymore — like a
janitor who throws away whatever is no longer referenced.

To avoid freezing the language/audio thread, the collector works **incrementally**
— a little at a time — using the classic "tri-color" method. Picture every object
wearing a colored hat:

- **white** = "maybe garbage; not checked yet"
- **grey** = "in use, but I haven't looked at what it points to yet"
- **black** = "in use, and everything it points to is accounted for"

The collector starts at the *roots* (the stack, global variables), paints them
grey, then repeats: take a grey object, paint everything it references grey, then
paint it black. When no grey objects remain, anything still white is unreachable
and gets collected. Then it starts again.

Two consequences of doing this *incrementally* are the **only two rules** a
primitive must respect:

### Rule 1 — the write barrier

Because collection happens in steps, the object graph can change mid-sweep.
Suppose the collector already finished an object (painted it **black**) and then
your primitive stores a reference to a brand-new **white** object inside it. The
collector considers the black object done and won't look at it again — so it
never discovers the white object and collects it while it's still in use →
crash.

The fix is a **write barrier**: whenever you store an object reference *into*
another object, you notify the GC "look again." In C++ you must remember to call
`GCWrite` every single time; forget once and you get a rare, vicious corruption
bug.

> **In this crate you cannot forget it.** The only way to put an element into an
> array is `ArrayBuilder::set`, which runs the barrier for you.

### Rule 2 — don't let a fresh object die before it's anchored

Allocating a new object can itself trigger a step of collection. If you create
object A, then allocate object B (which triggers a sweep) while A is reachable
*only* through a local Rust pointer the GC can't see, the GC may collect A out
from under you.

The fix is to pause collection while you build, then resume once your result is
reachable from a root.

> **In this crate that pause is the `Gc` value** the framework hands your
> primitive. While it exists, collection is suspended; when your function
> returns, it ends — and by then your result has been written to the stack,
> which *is* a root, so the object is safe.

That is the whole story. Both rules are about the **interpreter's** GC — not
Rust's ownership and not C++'s `new`/`delete`. That's why simply switching to
Rust wouldn't remove them, but a wrapper that bakes them in does.

What the wrapper can't hide (rare): keeping an sclang object reference on the
Rust side *after* the primitive returns, or a primitive that calls back into the
interpreter mid-computation. Both need explicit "rooting" and are out of scope
here.

---

## Anatomy of a primitive

Every primitive is four small pieces:

```rust
// 1. a plain Rust function with a fixed shape
fn nth_prime(args: &mut Args) -> Result<(), PrimError> {
    let n = args.arg(0).as_int()?;                  // arg(0) = the receiver (10)
    args.set_result(Value::Int(compute(n)));        // write the return value
    Ok(())
}

// 2. one macro line: name it, give its arg count, generate the C wrapper
sc_primitive!(NTH_PRIME, "_RustNthPrime", 1, nth_prime);  // 1 = just the receiver
```

```rust
// 3. register it (src/registry.rs)
define(math::NTH_PRIME);
```

```supercollider
// 4. expose it as a method (classes/RustExt.sc)
+ Integer { rustNthPrime { _RustNthPrime; ^this.primitiveFailed } }
```

`args.arg(0)` is always the receiver (`self`); `args.arg(1)` is the first
explicit argument, and so on. The count in the macro is the method's arity
*including* the receiver: `10.rustNthPrime` pushes just the receiver = 1; a
two-input method like `3.rustHypot(4)` pushes receiver + arg = 2.

Use `sc_primitive!` for value functions and `sc_primitive_gc!` for anything that
allocates — the latter passes your function an extra `&Gc` argument:

```rust
fn primes_up_to(args: &mut Args, gc: &Gc) -> Result<(), PrimError> { ... }
sc_primitive_gc!(PRIMES_UP_TO, "_RustPrimesUpTo", 1, primes_up_to);
```

---

## Walkthrough: `factorize`, line by line

All the example primitives live in [`sc-prim/src/prims/`](sc-prim/src/prims/) —
open them as you read. Let's walk through one real, complete primitive:
`360.rustFactorize` → `[2, 2, 2, 3, 3, 5]`. It takes a number (the receiver) and
returns an Array, so it exercises both argument reading *and* GC allocation.

**Piece 1 — the function** ([`sc-prim/src/prims/math.rs`](sc-prim/src/prims/math.rs)):

```rust
//                         ↓ &Gc, because we allocate an Array
pub fn factorize(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let mut n = args.arg(0).as_int()? as i64;   // arg(0) = the receiver (360);
                                                 // `?` returns errWrongType if not an Int

    // ----- plain Rust, no interpreter awareness at all -----
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

    // ----- hand the result back to sclang as an Array -----
    let mut arr = gc.new_array(factors.len())?; // allocate inside the paused-GC window
    for (i, f) in factors.iter().enumerate() {
        arr.set(i, Value::Int(*f));             // `set` runs the write barrier for us
    }
    args.set_result(arr.finish());              // write the Array into the result slot
    Ok(())
}
sc_primitive_gc!(FACTORIZE, "_RustFactorize", 1, factorize); // just the receiver = 1
```

What each part is doing, mapped to the [GC rules](#how-the-garbage-collector-works-plain-language):

- `gc` is the **collection-paused window** (Rule 2). It exists for the whole call,
  so the partly-built array can't be collected mid-construction. When the function
  returns, the window closes — but by then `set_result` has put the array on the
  stack, which the GC scans, so it's safe.
- `arr.set(...)` is the only way to fill the array, and it applies the **write
  barrier** (Rule 1) every time — you can't forget it.
- The actual maths is ordinary Rust. No `unsafe`, no GC bookkeeping in sight.

**Piece 2 — register it** ([`sc-prim/src/registry.rs`](sc-prim/src/registry.rs)):

```rust
define(math::FACTORIZE);
```

**Piece 3 — expose it as a method**
([`classes/RustExt.sc`](classes/RustExt.sc)):

```supercollider
+ Integer { rustFactorize { _RustFactorize; ^this.primitiveFailed } }
```

**Piece 4 — build & run.** Rebuild the lib (`cargo build --release` in `sc-prim/`),
rebuild sclang, then `360.rustFactorize` returns `[2, 2, 2, 3, 3, 5]`.

### Adding your own

It is the same four pieces. Say you want `x.rustClip(lo, hi)` — a *value-only*
primitive (no allocation), so use `sc_primitive!` and drop the `&Gc`:

```rust
pub fn clip(args: &mut Args) -> Result<(), PrimError> {
    let x  = args.arg(0).as_float()?;   // receiver
    let lo = args.arg(1).as_float()?;
    let hi = args.arg(2).as_float()?;
    args.set_result(Value::Float(x.clamp(lo, hi)));
    Ok(())
}
sc_primitive!(CLIP, "_RustClip", 3, clip); // receiver + 2 args = 3
```

…then register `math::CLIP`, add
`+ SimpleNumber { rustClip { |lo, hi| _RustClip; ^this.primitiveFailed } }`,
and rebuild. Compare with the other examples for arrays
([`prims/array.rs`](sc-prim/src/prims/array.rs)), signals
([`prims/signal.rs`](sc-prim/src/prims/signal.rs)), strings
([`prims/string.rs`](sc-prim/src/prims/string.rs)) and foreign objects
([`prims/foreign_demo.rs`](sc-prim/src/prims/foreign_demo.rs)).

---

## Under the hood: the C++ way vs. this crate

It's worth seeing *what a primitive actually does* and what writing one in plain
C++ costs, because that's exactly the overhead this crate removes.

**What a primitive is, mechanically.** It is a C function the interpreter calls
with two things: a pointer to the VM (`g`) and the number of values the caller
pushed. Those values sit on the interpreter's **operand stack** as 16-byte
*tagged slots* (a type tag + 8 bytes of payload). The primitive must:

1. find its arguments by doing pointer arithmetic on the stack pointer `g->sp`,
2. check each slot's tag and pull out the raw value,
3. do its work,
4. if it produces an object, allocate it on the **GC heap** and cooperate with
   the collector (write barriers, pausing collection — the two rules above),
5. write a tagged result back into the receiver's stack slot,
6. return an integer error code.

Every one of those steps is manual in C++. Here is `factorize` — the same
primitive we wrote above — done the idiomatic C++ way:

```cpp
int prFactorize(VMGlobals* g, int numArgsPushed) {
    PyrSlot* self = g->sp;                       // (1) receiver = top of stack
    if (NotInt(self)) return errWrongType;       // (2) manual tag check
    int64 n = slotRawInt(self);                  //     manual extraction

    std::vector<int> factors;                    // (3) the actual work
    for (int64 d = 2; d * d <= n; ++d)
        while (n % d == 0) { factors.push_back((int)d); n /= d; }
    if (n > 1) factors.push_back((int)n);

    // (4) build the result array, cooperating with the GC BY HAND:
    g->gc->enterDelayedCollectionContext();           // pause collection (Rule 2)
    PyrObject* arr = newPyrArray(g->gc, factors.size(), 0, true);  // alloc, get flags right
    for (size_t i = 0; i < factors.size(); ++i) {
        SetInt(arr->slots + i, factors[i]);           // manual tagging
        g->gc->GCWrite(arr, arr->slots + i);          // manual write barrier (Rule 1)
    }
    arr->size = factors.size();                       // set the size by hand
    g->gc->exitDelayedCollectionContext();            // resume collection
    SetObject(self, arr);                             // (5) result into receiver slot
    return errNone;                                   // (6) error code
}
```

…plus, elsewhere, a hand-written `definePrimitive("_RustFactorize", prFactorize, 1, 0)`
in an init function, and the `.sc` method. And note what is *not* on the page:

- **No type help.** Forget a `NotInt`/`slotRawInt` mismatch and you read garbage.
- **No safety net.** A C++ exception or a stray bug doesn't return an error — it
  crashes the whole interpreter.
- **Silent, delayed failure modes.** Forget the `GCWrite`, or get the
  `enterDelayedCollectionContext` bracket wrong, and nothing fails *now* — you get
  heap corruption that crashes minutes later, somewhere unrelated. These are the
  bugs that make C++ primitives miserable to write and review.

The Rust version (the walkthrough above) expresses the same six steps, but the
error-prone ones are gone — folded into the types:

| step | C++ primitive | this crate |
|---|---|---|
| read receiver | `g->sp` + `NotInt` + `slotRawInt` | `args.arg(0).as_int()?` |
| wrong type | `return errWrongType;` | the `?` |
| pause collection | `enter/exitDelayedCollectionContext()`, bracket by hand | the `&Gc` scope (RAII) |
| allocate | `newPyrArray(gc, n, flags, true)` | `gc.new_array(n)?` |
| set element | `SetInt(slot, v)` | `arr.set(i, Value::Int(v))` |
| **write barrier** | `g->gc->GCWrite(arr, slot)` — *must not forget* | applied inside `set` |
| set size | `arr->size = n` by hand | done by `finish()` |
| return value | `SetObject(self, arr)` | `args.set_result(arr.finish())` |
| a panic / exception | crashes the interpreter | caught → `errFailed` |
| forgotten barrier / bad scope | heap corruption later | not expressible |

The crate doesn't make the GC simpler — the rules are identical. It moves the two
rules from "things you must remember every time, with crashes if you don't" to
"things the API does for you, that you can't get wrong." That is the whole value.

---

## Reading arguments and returning values

`args.arg(i)` returns a `Slot` you can decode:

| method | returns | on type mismatch |
|---|---|---|
| `.as_int()` | `i32` | `Err(WRONG_TYPE)` |
| `.as_float()` | `f64` (ints coerced) | `Err(WRONG_TYPE)` |
| `.as_str()` | `&str` (a String arg) | `Err(WRONG_TYPE)` |
| `.as_f32_slice()` | `&[f32]` (a Signal) | `Err(WRONG_TYPE)` |
| `.as_f64_vec()` | `Vec<f64>` (an Array of numbers) | `Err(WRONG_TYPE)` |
| `.value()` | the `Value` enum | never |

The `?` operator turns a mismatch into the right sclang error automatically.
Return a value with `args.set_result(Value::...)`:

```rust
Value::Int(42) | Value::Float(1.5) | Value::Bool(true) | Value::Nil
```

---

## Allocating objects: arrays, strings, signals

Take a `&Gc` (via `sc_primitive_gc!`) and build through it. The `Gc` is the
"collection paused" window from Rule 2; the builders apply the write barrier from
Rule 1.

```rust
// Array of ints
let mut arr = gc.new_array(items.len())?;
for (i, v) in items.iter().enumerate() { arr.set(i, Value::Int(*v)); }
args.set_result(arr.finish());

// String
args.set_result(gc.new_string("hello")?);

// Signal (float array) — fill the slice directly
let mut sig = gc.new_signal(n)?;
for (i, s) in sig.as_mut_slice().iter_mut().enumerate() {
    *s = (TAU * i as f32 / n as f32).sin();
}
args.set_result(sig.finish());
```

See [`prims/array.rs`](sc-prim/src/prims/array.rs) (`rustPrimesUpTo`,
`rustHistogram`) and [`prims/signal.rs`](sc-prim/src/prims/signal.rs)
(`rustSine`, `rustNormalize`, `rustRms`) for complete examples.

---

## Owning a Rust value from sclang (foreign objects)

Sometimes you want a *Rust* object — a file handle, a network connection, a big
data structure — to live as long as an sclang object. Use `foreign::attach`: it
boxes your value on the Rust heap, stashes the pointer in the sclang object, and
installs a **finalizer** so the value's `Drop` runs when the sclang object is
collected. You still never call `free`.

```rust
pub struct Counter { label: String, count: i64 }
impl Drop for Counter {                              // ordinary Rust cleanup
    fn drop(&mut self) { /* close files, etc. */ }
}

// `RustCounter("name")` -> attach a Counter to self
pub fn counter_new(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let label = args.arg(1).as_str().unwrap_or("counter").to_string();
    unsafe { foreign::attach(args.vm(), obj, Counter { label, count: 0 }) };
    args.set_result(Value::Obj(obj));
    Ok(())
}

// borrow it
pub fn counter_next(args: &mut Args) -> Result<(), PrimError> {
    let obj = args.receiver_obj()?;
    let n = unsafe { foreign::with_mut::<Counter, _>(obj, |c| { c.count += 1; c.count }) }
        .ok_or(PrimError::FAILED)?;
    args.set_result(Value::Int(n as i32));
    Ok(())
}

// free it eagerly (optional; otherwise the finalizer does it at GC time)
pub fn counter_free(args: &mut Args) -> Result<(), PrimError> {
    unsafe { foreign::take::<Counter>(args.receiver_obj()?) }; // Drop runs here
    args.set_result(Value::Nil);
    Ok(())
}
```

**Convention:** the sclang class must reserve its **first two instance variables**
for the pointer and finalizer:

```supercollider
RustCounter {
    var ptr;        // slot 0 — managed by the primitive
    var finalizer;  // slot 1 — managed by the primitive
    var <label;     // your fields after
    ...
}
```

`take` drops immediately and nils the pointer, so the finalizer becomes a no-op:
no double free. The `no_double_free` test verifies this.

---

## Errors and panics

Return `Err(PrimError::WRONG_TYPE)` (or `FAILED`, `OUT_OF_MEMORY`, …) and the
interpreter runs the method's fallback. The `?` operator does this for you on a
type mismatch.

If your Rust code *panics*, the wrapper catches it at the FFI boundary, posts a
message, and returns `errFailed` — a panic can never unwind into the C++
interpreter and crash it.

---

## Example: an HTTP request

Doing HTTP in a C++ primitive means sockets or linking curl. In Rust it's a
crate. [`prims/http.rs`](sc-prim/src/prims/http.rs) (behind the `http` Cargo
feature) is:

```rust
pub fn http_get(args: &mut Args, gc: &Gc) -> Result<(), PrimError> {
    let url = args.arg(0).as_str()?;   // receiver is the URL string
    let body = ureq::get(url).timeout(Duration::from_secs(10)).call()
        .ok().and_then(|r| r.into_string().ok());
    match body {
        Some(s) => args.set_result(gc.new_string(&s)?),
        None    => args.set_result(Value::Nil),
    }
    Ok(())
}
```

Build it with `cargo build --release --features http`, then
`"http://example.com".rustHttpGet`.

> **Caveat — blocking.** This call blocks the language thread until it completes
> or times out; sclang is frozen meanwhile. Fine for scripting, not for live use.
> A production version would run the request on a background thread (`std::thread`
> or an async runtime) and deliver the result back to sclang asynchronously — for
> example by storing it and signalling a registered callback / a polled flag.
> That requires "rooting" the target object across the call, which is the one
> area this crate intentionally leaves to you (see Rule notes above).

---

## Testing and checking for leaks

Two layers, both run by `cargo test`:

- **Behavior** — each primitive is driven through an in-process mock host
  ([`src/test_host.rs`](sc-prim/src/test_host.rs)) and its result checked
  ([`src/tests.rs`](sc-prim/src/tests.rs)).
- **Memory safety** — a `LeakProbe` type increments a counter on creation and
  decrements on `Drop`. The tests drive the foreign-object machinery thousands of
  times and assert the live count returns to **zero** (no leak) and never goes
  **negative** (no double free): `no_leak_on_explicit_free`,
  `no_leak_on_finalizer_collection`, `no_double_free`.

```sh
cd sc-prim && cargo test          # behavior + leak tests
./build.sh                        # also builds & runs the standalone C++ demo
```

For an extra check at the C boundary, the mock demo frees all its objects at
exit, so it runs clean under valgrind:

```sh
valgrind --leak-check=full ./build/mock_demo
```
