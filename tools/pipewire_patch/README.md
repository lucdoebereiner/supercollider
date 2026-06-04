# PipeWirePatch — PipeWire graph control from sclang

Query the PipeWire graph and connect/disconnect scsynth's PipeWire client to
anything else on it, entirely from sclang. Pure sclang — it shells out to the
`pw-link` CLI (blocking, so operations happen in order); **no C++ primitives
and no scsynth changes required**, because PipeWire's graph is managed
out-of-process.

Pairs with the local experimental PipeWire backend
(`server/scsynth/SC_PipeWire.cpp`), which names each server's nodes
`"<clientName> playback"` / `"<clientName> capture"` (grouped under
`application.name = <clientName>`). `<clientName>` comes from `s.options.device`
— so giving each server a distinct device string lets `PipeWirePatch` target
multiple servers independently.

## Requirements

- `pw-link` on `PATH` (from `pipewire` / `pipewire-utils`).
- scsynth built with the PipeWire backend (`-DAUDIOAPI=pipewire`).

## Install

```sh
./install.sh              # symlink into the per-user Extensions dir (edits live)
./install.sh --copy       # copy instead of symlink
./install.sh --system     # system-wide Extensions dir
./install.sh --uninstall  # remove
SC_EXTENSIONS_DIR=/path ./install.sh   # override target dir
```

Then recompile the class library (IDE: *Lang ▸ Recompile Class Library*) or
restart sclang. With the default symlink, editing `PipeWirePatch.sc` in this
repo takes effect on the next recompile — no reinstall.

## Usage

```supercollider
// Graph-wide queries (return arrays of PipeWirePort / PipeWireLink):
PipeWire.nodes;        // distinct node names
PipeWire.outPorts;     // all output ports
PipeWire.inPorts;      // all input ports
PipeWire.links;        // all links (deduped, with stable ids)
PipeWire.report;       // pretty-print nodes (with out/in counts) + links
PipeWire.portsOf("alsa_output.usb-...pro-output-0");

// Any node's ports in channel order — `.size` is the channel count,
// `[i]` is channel i. Works for any client, so you can index + count them:
PipeWire.outPortsOf("alsa_input.usb-...pro-input-0");   // .size = how many
PipeWire.inPortsOf("alsa_output.usb-...pro-output-0");

// Low-level link ops (blocking, return Boolean):
PipeWire.connect("ScA playback:output_AUX0", "alsa_output...:playback_AUX0");
PipeWire.disconnect(outName, inName);
PipeWire.disconnectLink(113);   // by link id

// Server-bound convenience:
s.options.device = "ScA";       // set BEFORE s.boot so the nodes get this name
~a = PipeWirePatch(s);          // or PipeWirePatch("ScA") by name
~a.report;
~a.outputs; ~a.inputs; ~a.monitors;   // channel-ordered PipeWirePort arrays
~a.numOutputs; ~a.numInputs;           // channel counts (== .outputs.size etc.)
~a.outputs[0];                         // a specific channel's PipeWirePort
~a.links;                              // links touching either of its nodes

// Bulk routing — ports paired in channel order (target wraps if it has fewer).
// Returns the number of links made/removed.
~a.connectOutputsTo("alsa_output.usb-...pro-output-0");  // node name
~a.connectOutputsTo(~b);          // another PipeWirePatch or Server
~a.connectInputsFrom("alsa_input.usb-...pro-input-0");
~a.disconnectOutputs;
~a.disconnectInputs;
~a.disconnectAll;

// Single port-to-port routing by channel index (returns Boolean).
// dstIndex/srcIndex default to the other index when omitted.
~a.connectOutput(0, "alsa_output.usb-...pro-output-0", 1); // our out 0 -> sink in 1
~a.connectOutput(1, ~b, 3);          // our out 1 -> ~b's input 3
~a.connectInput(0, "alsa_input.usb-...pro-input-0", 2);    // src out 2 -> our in 0
~a.disconnectOutput(0, "alsa_output.usb-...pro-output-0", 1);
~a.disconnectInput(0, ~b, 2);

// Multiple servers — distinct device names, target each independently:
// (~b = PipeWirePatch(s2); ~a.connectOutputsTo(~b);)
```

### Targets / sources accepted by `connectOutputsTo` / `connectInputsFrom`

A node-name `String`, a `Server`, a `PipeWirePatch`, a `PipeWirePort`, or an
`Array` of any of those.

### Multiple PipeWire daemons

```supercollider
PipeWire.remoteName = "my-remote";   // pw-link --remote=NAME; nil = default
```

## Notes / limits

- **Blocking by design** — every call finishes (and the graph settles) before
  the next line runs, so sequences of connects/disconnects are deterministic.
  It does briefly block the language thread per `pw-link` invocation; fine for
  interactive patching, avoid in tight loops.
- Node/port split is done at the last `:` (port names never contain one).
- For multi-server use, give each server a unique `options.device`; two servers
  sharing a name produce ambiguous nodes.
