// Module A of the two-module test. Compiled to its own object under its own
// entry symbol; tests/two_module/host.cpp is what enters it and says why.
//
// A publishes state, a class, a closure pair and a guarded caller on
// globalThis. Everything module B later reaches, it reaches BY NAME — so every
// line B prints is only right if the two objects' key strings interned to one
// process-wide id.

const registry = { count: 0, items: ["alpha"], alphaOnly: true };

class Shape {
  constructor(name) { this.name = name; }
  describe() { return "Shape(" + this.name + ")"; }
}

// Top-level function declarations: no environment parameter, so each reads the
// module scope out of THIS module's environment cell. B calls them after B's
// own top level has run, which is exactly when a single process-wide module
// environment would hand them B's bindings.
function bump(n) { registry.count += n; return registry.count; }

let secret = "a0";
function readSecret() { return secret; }
function writeSecret(v) { secret = v; }

// The `try` that catches B's throw is compiled into THIS object. The pending
// exception cell is the one thing here that stays process-wide, and this is
// what proves it still is.
function callGuarded(f) {
  try { return "ok(" + f() + ")"; }
  catch (e) { return "caught(" + e.name + ": " + e.message + ")"; }
}

globalThis.registry = registry;
globalThis.Shape = Shape;
globalThis.bump = bump;
globalThis.readSecret = readSecret;
globalThis.writeSecret = writeSecret;
globalThis.callGuarded = callGuarded;

// `alphaOnly` is a key only this module has; `betaOnly` is one only B has, and
// reading it here — before B exists — is the divergent half of the key sets.
console.log("A count=" + registry.count + " items=" + registry.items.join(",") +
            " secret=" + readSecret());
console.log("A " + new Shape("first").describe() +
            " alphaOnly=" + registry.alphaOnly + " betaOnly=" + registry.betaOnly);
