// The module of the native shared-load test: compiled by `bronze build
// --emit-shared --native-manifest` against the manifest manifest_tool.cpp
// printed, opened at run time by tests/native/harness.cpp, which registers
// the natives, binds this module's import table, and only then enters it.
//
// Every native call below was lowered to a direct call through an import
// slot at compile time, from a manifest — the registry that filled the slot
// is the harness's, at load time. Nothing here was resolved by a linker.

console.log("module: add=" + nt.math.add(2, 3) + " bits=" + nt.math.bits(12, 10) +
            " flip=" + nt.math.flip(false));
console.log("module: greet=" + nt.str.greet("shared") + " len=" + nt.str.len("four"));
console.log("module: sumU16=" + nt.arr.sumU16(new Uint16Array([1, 2, 3])) +
            " sumF64=" + nt.arr.sumF64(new Float64Array([0.5, 0.25])));

const agent = new nt.Agent(30);
agent.hp -= 5;
console.log("module: agent hp=" + agent.hp + " hit=" + agent.hit(5) + " id=" + agent.id() +
            " target=" + agent.target().value() + " label=" + agent.label());

nt.time.scale = 4;
console.log("module: scale=" + nt.time.scale);

try {
  nt.peek({});
} catch (e) {
  console.log("module: " + e.name + ": " + e.message);
}
try {
  nt.arr.sumF32(new Int8Array(1));
} catch (e) {
  console.log("module: " + e.name + ": " + e.message);
}

// A host global, read through the module's cache cell: the harness reads it
// back through readTick before and after re-registering it.
console.log("module: tick=" + hostTick);
globalThis.readTick = function () { return hostTick; };

globalThis.makeAgents = function (n) {
  for (let i = 0; i < n; i++) new nt.Agent(i);
};
globalThis.sumHost = function (view) { return nt.arr.sumF32(view); };
globalThis.agentHp = function () { return agent.hp; };
