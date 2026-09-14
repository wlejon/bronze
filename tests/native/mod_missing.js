// Compiled against a manifest that has nt.absent.ping, which the harness
// never registers. The harness opens this module, reads its import table,
// asks the runtime to bind it, and is refused with the name — before the
// entry runs. The entry would be FATAL on the same gap (its first call is
// the bind), so the pinned output is the proof it was not entered: this
// line must never appear.
console.log("module: absent=" + nt.absent.ping());
