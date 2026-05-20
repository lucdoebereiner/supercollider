// RustExt.sc — sclang glue for the example Rust primitives (see ../sc-rust-prim).
//
// These are EXAMPLES, so the methods are prefixed `rust...` to avoid clashing
// with existing SuperCollider methods (hypot, reverse, normalize already exist).
// Each runs its primitive on the receiver; `^this.primitiveFailed` is the
// fallback if the primitive errors (wrong type, or the lib wasn't built with it).
//
// When the lib is built with SC_RUST_PRIMITIVES=ON, CMake installs this file into
// share/SuperCollider/Extensions so the installed sclang loads it automatically.
//
//   10.rustNthPrime;                 // 29
//   360.rustFactorize;               // [ 2, 2, 2, 3, 3, 5 ]
//   30.rustPrimesUpTo;               // [ 2, 3, 5, 7, ... ]
//   3.rustHypot(4);                  // 5.0
//   [0, 0.1, 0.9, 1].rustHistogram(2);
//   Signal.rustSine(512).plot;       // create a Signal (class method)
//   Signal.sineFill(64, [1]).rustNormalize.rustRms;
//   "hello".rustReverse;             // "olleh"
//   "hi".rustShout;                  // "HI!"
//   "http://example.com".rustHttpGet // needs the lib built with --features http

+ Integer {
	rustNthPrime   { _RustNthPrime;   ^this.primitiveFailed }  // 10.rustNthPrime
	rustFactorize  { _RustFactorize;  ^this.primitiveFailed }  // 360.rustFactorize
	rustPrimesUpTo { _RustPrimesUpTo; ^this.primitiveFailed }  // 30.rustPrimesUpTo
}

+ SimpleNumber {
	rustHypot { |aNumber| _RustHypot; ^this.primitiveFailed }  // 3.rustHypot(4)
}

+ ArrayedCollection {
	// receiver is an Array of numbers
	rustHistogram { |nbins = 8| _RustHistogram; ^this.primitiveFailed }
}

+ Signal {
	rustNormalize { _RustNormalizeSignal; ^this.primitiveFailed } // -> new Signal, peak 1.0
	rustRms       { _RustSignalRms;       ^this.primitiveFailed } // -> Float
	*rustSine     { |size| _RustSineSignal; ^this.primitiveFailed } // one cycle of a sine
}

+ String {
	rustReverse { _RustReverseString; ^this.primitiveFailed }  // "abc".rustReverse
	rustShout   { _RustShout;         ^this.primitiveFailed }  // "hi".rustShout
	rustHttpGet { _RustHttpGet;       ^this.primitiveFailed }  // url.rustHttpGet (feature: http)
}

// A Rust object (Counter) owned by an sclang object — a genuinely new type, so it
// stays its own class. The first two instance variables are reserved for the Rust
// pointer and finalizer (managed by the primitives); add your own fields after.
RustCounter {
	var ptr;        // slot 0 — Rust heap pointer
	var finalizer;  // slot 1 — finalizer reference
	var <label;     // slot 2+ — your fields

	*new { |label = "counter"| ^super.new.prInit(label) }
	prInit { |label| _RustCounterNew; ^this.primitiveFailed }
	next { _RustCounterNext; ^this.primitiveFailed }   // -> new count
	free { _RustCounterFree; ^this.primitiveFailed }   // drop the Rust object now
}
