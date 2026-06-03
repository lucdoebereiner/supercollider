// PipeWire graph control for SuperCollider — query nodes/ports/links and
// connect/disconnect scsynth's PipeWire client to anything else on the
// graph, entirely from sclang.
//
// This is a pure-sclang helper: it shells out to the `pw-link` CLI via
// `String.unixCmdGetStdOut` (blocking, so operations happen in order). It
// requires no C++ primitives and no changes to scsynth — PipeWire's graph
// is managed out-of-process, so any client can rewire it.
//
// It pairs with the local experimental PipeWire backend
// (server/scsynth/SC_PipeWire.cpp): that backend names each server's nodes
// "<clientName> playback" / "<clientName> capture" and groups them under
// application.name = <clientName>, where <clientName> comes from
// `s.options.device` (or inDevice/outDevice). Give each server a distinct
// device string and PipeWirePatch can target them independently:
//
//     s.options.device = "scA";  // before s.boot
//     ~a = PipeWirePatch(s);
//     ~a.outputs;                // its playback output ports
//     ~a.connectOutputsTo("alsa_output.usb-...pro-output-0");
//     ~a.disconnectOutputs;
//     PipeWire.nodes;            // everything on the graph
//
// Requires `pw-link` on PATH (part of pipewire-utils / pipewire).

// ---------------------------------------------------------------------
// Value objects

PipeWirePort {
	var <id, <fullName, <node, <port, <direction; // direction: \output or \input

	*new { |id, fullName, node, port, direction|
		^super.newCopyArgs(id, fullName, node, port, direction)
	}

	printOn { |stream|
		stream << "PipeWirePort(" << id << ", " << fullName.asCompileString
		<< ", " << direction << ")"
	}
}

PipeWireLink {
	var <id, <output, <input; // output/input are full "node:port" name strings

	*new { |id, output, input|
		^super.newCopyArgs(id, output, input)
	}

	printOn { |stream|
		stream << "PipeWireLink(" << id << ", " << output.asCompileString
		<< " -> " << input.asCompileString << ")"
	}
}

// ---------------------------------------------------------------------
// PipeWire — class-side graph queries + link operations (wraps pw-link)

PipeWire {
	classvar <>program = "pw-link";
	classvar <>remoteName; // nil = default daemon; set to use --remote=NAME

	// --- public queries ------------------------------------------------

	*outPorts { ^this.prParsePorts(this.prRun(["-I", "-o"]), \output) }
	*inPorts  { ^this.prParsePorts(this.prRun(["-I", "-i"]), \input) }
	*ports    { ^this.outPorts ++ this.inPorts }

	*nodes {
		^this.ports.collect(_.node).as(Set).asArray.sort
	}

	*portsOf { |nodeName|
		^this.ports.select { |p| p.node == nodeName }
	}

	*links {
		var lines = this.prRun(["-I", "-l"]).split($\n);
		var ctxName, byId = Dictionary.new;
		lines.do { |line|
			if(this.prIsLinkLine(line)) {
				var t1 = this.prCutToken(line);          // [linkId, rest]
				var linkId = t1[0].asInteger;
				var rest = t1[1];                        // "|-> <peerId> <peerName>"
				var arrow = rest.keep(3);                // "|->" or "|<-"
				var t2 = this.prCutToken(rest.drop(3));  // [peerId, peerName]
				var peerName = t2[1];
				var outName, inName;
				if(arrow == "|->") {
					outName = ctxName; inName = peerName;
				} {
					outName = peerName; inName = ctxName;
				};
				byId[linkId] = PipeWireLink(linkId, outName, inName);
			} {
				var l = this.prTrim(line);
				if(l.notEmpty) {
					ctxName = this.prCutToken(l)[1];     // context port full name
				};
			};
		};
		^byId.values.asArray.sort { |a, b| a.id < b.id }
	}

	// --- public operations (blocking; return Boolean success) ----------

	*connect { |outName, inName|
		var r = this.prSystem(["-L", outName.asString, inName.asString]);
		if(r[\code] != 0) { ("PipeWire.connect failed: " ++ r[\output]).warn };
		^(r[\code] == 0)
	}

	*disconnect { |outName, inName|
		var r = this.prSystem(["-d", outName.asString, inName.asString]);
		if(r[\code] != 0) { ("PipeWire.disconnect failed: " ++ r[\output]).warn };
		^(r[\code] == 0)
	}

	*disconnectLink { |linkId|
		var r = this.prSystem(["-d", linkId.asString]);
		if(r[\code] != 0) { ("PipeWire.disconnectLink failed: " ++ r[\output]).warn };
		^(r[\code] == 0)
	}

	*report {
		"PipeWire graph (remote: %)".format(remoteName ? "default").postln;
		"  nodes:".postln;
		this.nodes.do { |n| ("    " ++ n).postln };
		"  links:".postln;
		this.links.do { |lk|
			("    " ++ lk.output ++ "  ->  " ++ lk.input ++ "   [id " ++ lk.id ++ "]").postln
		};
		^this
	}

	// --- internal: command execution -----------------------------------

	*prCmdString { |argsArray|
		var parts = [program];
		if(remoteName.notNil) { parts = parts.add("--remote=" ++ remoteName) };
		parts = parts ++ argsArray;
		^parts.collect { |a| this.prQuote(a.asString) }.join(" ")
	}

	*prRun { |argsArray|
		^(this.prCmdString(argsArray) ++ " 2>/dev/null").unixCmdGetStdOut
	}

	// Returns an Event (code:, output:). Trailing "__RC<n>__" marker is
	// parsed off so we can report the exit status of pw-link.
	*prSystem { |argsArray|
		var out, lines, rcLine, code, body;
		out = (this.prCmdString(argsArray) ++ " 2>&1; echo \"__RC$?__\"").unixCmdGetStdOut;
		lines = out.split($\n);
		rcLine = lines.reverse.detect { |l| l.contains("__RC") };
		code = if(rcLine.notNil) {
			rcLine.replace("__RC", "").replace("__", "").asInteger
		} { -1 };
		body = lines.reject { |l| l.contains("__RC") }.join("\n");
		^(code: code, output: this.prTrim(body))
	}

	*prQuote { |str|
		^"'" ++ str.replace("'", "'\\''") ++ "'"
	}

	// --- internal: parsing ---------------------------------------------

	*prParsePorts { |raw, direction|
		^raw.split($\n).collect { |line|
			var l = this.prTrim(line), t, np;
			if(l.isEmpty) {
				nil
			} {
				t = this.prCutToken(l);            // [id, fullName]
				np = this.prSplitPort(t[1]);       // [node, port]
				PipeWirePort(t[0].asInteger, t[1], np[0], np[1], direction)
			}
		}.reject(_.isNil)
	}

	*prIsLinkLine { |line|
		^(line.contains("|->") or: { line.contains("|<-") })
	}

	// Split a node:port string into [node, port] at the LAST colon (port
	// names never contain ':', but some node names do, e.g. Midi-Bridge).
	*prSplitPort { |name|
		var i = name.size - 1;
		while { (i >= 0) and: { name[i] != $: } } { i = i - 1 };
		^if(i < 0) {
			[name, ""]
		} {
			[name.copyRange(0, i - 1), name.copyRange(i + 1, name.size - 1)]
		}
	}

	// Trailing integer of a port name (for stable channel ordering); -1 if none.
	*prPortNumber { |port|
		var i = port.size - 1;
		while { (i >= 0) and: { port[i].isDecDigit } } { i = i - 1 };
		^if(i == (port.size - 1)) { -1 } { port.copyRange(i + 1, port.size - 1).asInteger }
	}

	// Cut the first whitespace-delimited token: returns [token, trimmedRemainder].
	*prCutToken { |str|
		var s = this.prTrim(str), i = 0;
		while { (i < s.size) and: { (s[i] != $ ) and: { s[i] != $\t } } } { i = i + 1 };
		^[s.copyRange(0, i - 1), this.prTrim(s.copyRange(i, s.size - 1))]
	}

	*prTrim { |str|
		var a = 0, b = str.size - 1;
		while { (a <= b) and: { this.prIsWS(str[a]) } } { a = a + 1 };
		while { (b >= a) and: { this.prIsWS(str[b]) } } { b = b - 1 };
		^if(a > b) { "" } { str.copyRange(a, b) }
	}

	*prIsWS { |ch|
		^(ch == $ ) or: { ch == $\t } or: { ch == $\n } or: { ch == $\r }
	}
}

// ---------------------------------------------------------------------
// PipeWirePatch — a server's PipeWire client, with convenience routing

PipeWirePatch {
	var <server, <clientName;

	// arg may be a Server (name derived from its options) or a String
	// (used directly as the client name).
	*new { |server|
		^super.new.prInit(server ? Server.default)
	}

	prInit { |obj|
		case
		{ obj.isKindOf(Server) } {
			server = obj;
			clientName = PipeWirePatch.prClientName(obj);
		}
		{ obj.isKindOf(String) } {
			server = nil;
			clientName = obj;
		}
		{ true } {
			Error("PipeWirePatch: expected a Server or String, got %".format(obj)).throw
		};
		^this
	}

	// Mirror SC_PipeWire.cpp parse_device_field: client name is the part of
	// the device string before the first ':' (whole string if none); prefer
	// inDevice, fall back to outDevice, default "SuperCollider".
	*prClientName { |server|
		var inN = this.prNamePrefix(server.options.inDevice);
		var outN = this.prNamePrefix(server.options.outDevice);
		^(inN ?? { outN }) ? "SuperCollider"
	}

	*prNamePrefix { |dev|
		var i;
		if(dev.isKindOf(String).not or: { dev.isEmpty }) { ^nil };
		i = dev.indexOf($:);
		^if(i.isNil) {
			dev
		} {
			if(i == 0) { nil } { dev.copyRange(0, i - 1) }
		}
	}

	outNodeName { ^clientName ++ " playback" }
	inNodeName  { ^clientName ++ " capture" }

	// our server's output ports (its DSP outputs), channel-ordered
	outputs {
		var n = this.outNodeName;
		^PipeWire.outPorts.select { |p| p.node == n }
			.sort { |a, b| PipeWire.prPortNumber(a.port) < PipeWire.prPortNumber(b.port) }
	}

	// our server's capture input ports (its DSP inputs), channel-ordered
	inputs {
		var n = this.inNodeName;
		^PipeWire.inPorts.select { |p| p.node == n }
			.sort { |a, b| PipeWire.prPortNumber(a.port) < PipeWire.prPortNumber(b.port) }
	}

	// the capture node's monitor output ports (a copy of what was captured)
	monitors {
		var n = this.inNodeName;
		^PipeWire.outPorts.select { |p| p.node == n }
			.sort { |a, b| PipeWire.prPortNumber(a.port) < PipeWire.prPortNumber(b.port) }
	}

	// all links touching either of our nodes
	links {
		var on = this.outNodeName, inn = this.inNodeName;
		^PipeWire.links.select { |lk|
			[lk.output, lk.input].any { |full|
				var node = PipeWire.prSplitPort(full)[0];
				(node == on) or: { node == inn }
			}
		}
	}

	// --- routing (blocking; return number of links made/removed) -------

	// Connect this server's outputs to a destination's inputs. target may be
	// a node-name String, a Server, a PipeWirePatch, a PipeWirePort, or an
	// Array of those. Ports are paired in channel order (dest wraps if shorter).
	connectOutputsTo { |target|
		^this.prZipConnect(this.outputs, this.prResolveInputs(target), "destination")
	}

	// Connect a source's outputs into this server's capture inputs.
	connectInputsFrom { |source|
		^this.prZipConnect(this.prResolveOutputs(source), this.inputs, "source")
	}

	prZipConnect { |srcs, dsts, what|
		var made = 0;
		if(srcs.isEmpty) { "PipeWirePatch: no source ports to connect".warn; ^0 };
		if(dsts.isEmpty) { ("PipeWirePatch: no % ports to connect to".format(what)).warn; ^0 };
		srcs.do { |s, i|
			if(PipeWire.connect(s.fullName, dsts.wrapAt(i).fullName)) { made = made + 1 };
		};
		^made
	}

	disconnectOutputs {
		var on = this.outNodeName, made = 0;
		this.links.select { |lk| PipeWire.prSplitPort(lk.output)[0] == on }.do { |lk|
			if(PipeWire.disconnectLink(lk.id)) { made = made + 1 };
		};
		^made
	}

	disconnectInputs {
		var inn = this.inNodeName, made = 0;
		this.links.select { |lk| PipeWire.prSplitPort(lk.input)[0] == inn }.do { |lk|
			if(PipeWire.disconnectLink(lk.id)) { made = made + 1 };
		};
		^made
	}

	disconnectAll {
		var made = 0;
		this.links.do { |lk| if(PipeWire.disconnectLink(lk.id)) { made = made + 1 } };
		^made
	}

	report {
		"PipeWirePatch: client '%' %".format(
			clientName,
			if(server.notNil) { "(server " ++ server.name ++ ")" } { "" }
		).postln;
		"  outputs (%):".format(this.outputs.size).postln;
		this.outputs.do { |p| ("    " ++ p.fullName).postln };
		"  inputs (%):".format(this.inputs.size).postln;
		this.inputs.do { |p| ("    " ++ p.fullName).postln };
		"  links (%):".format(this.links.size).postln;
		this.links.do { |lk|
			("    " ++ lk.output ++ "  ->  " ++ lk.input ++ "   [id " ++ lk.id ++ "]").postln
		};
		^this
	}

	// --- internal: resolve a target/source to a list of ports ----------

	prResolveInputs { |target|
		^case
		{ target.isKindOf(PipeWirePatch) } { target.inputs }
		{ target.isKindOf(Server) } { PipeWirePatch(target).inputs }
		{ target.isKindOf(PipeWirePort) } { [target] }
		{ target.isKindOf(String) } { PipeWire.inPorts.select { |p| p.node == target } }
		{ target.isKindOf(SequenceableCollection) } {
			target.collect { |t| this.prResolveInputs(t) }.flatten
		}
		{ true } { Error("PipeWirePatch: cannot resolve destination %".format(target)).throw }
	}

	prResolveOutputs { |source|
		^case
		{ source.isKindOf(PipeWirePatch) } { source.outputs }
		{ source.isKindOf(Server) } { PipeWirePatch(source).outputs }
		{ source.isKindOf(PipeWirePort) } { [source] }
		{ source.isKindOf(String) } { PipeWire.outPorts.select { |p| p.node == source } }
		{ source.isKindOf(SequenceableCollection) } {
			source.collect { |t| this.prResolveOutputs(t) }.flatten
		}
		{ true } { Error("PipeWirePatch: cannot resolve source %".format(source)).throw }
	}
}
