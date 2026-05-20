// SCRustPrim.sc — sclang class glue for the Rust primitives.
//
// Drop this file into your SuperCollider extensions directory
// (Platform.userExtensionDir) of a build that includes the Rust shim, then:
//
//     RustPrim.nthPrime(10);          // -> 29
//     RustPrim.primesUpTo(30);        // -> [ 2, 3, 5, 7, 11, ... ]
//     RustPrim.reverseString("abc");  // -> "cba"
//
//     c = RustCounter("voices");
//     c.next; c.next;                 // -> 1, 2
//     c.free;                         // Rust Drop runs now (or at GC if omitted)
//
// A primitive operates on the receiver and the pushed arguments. `^this.primitiveFailed`
// is the fallback the interpreter runs only if the primitive returns an error.

RustPrim {
	*nthPrime { |n|        _RustNthPrime;       ^this.primitiveFailed }
	*hypot { |a, b|        _RustHypot;          ^this.primitiveFailed }
	*primesUpTo { |n|      _RustPrimesUpTo;     ^this.primitiveFailed }
	*histogram { |data, nbins| _RustHistogram;  ^this.primitiveFailed }
	*reverseString { |str| _RustReverseString;  ^this.primitiveFailed }
	*shout { |str|         _RustShout;          ^this.primitiveFailed }
}

// A Rust object (Counter) owned by this sclang object.
//
// IMPORTANT: the foreign-object convention reserves the FIRST TWO instance
// variables for the Rust pointer and the finalizer. Declare them first and do
// not touch them from sclang; add your own ivars after.
RustCounter {
	var ptr;        // slot 0 — Rust heap pointer (managed by the primitive)
	var finalizer;  // slot 1 — finalizer reference (managed by the primitive)
	var <label;     // slot 2+ — your own fields go here

	*new { |label = "counter"| ^super.new.prInit(label) }

	prInit { |label| _RustCounterNew; ^this.primitiveFailed }

	// Increment and return the new count.
	next { _RustCounterNext; ^this.primitiveFailed }

	// Free the Rust object now (deterministic). Safe to call once; if you never
	// call it, the Rust Drop still runs when this object is garbage-collected.
	free { _RustCounterFree; ^this.primitiveFailed }
}
