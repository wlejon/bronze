// Hot-swap module, version 1. Compiled with --emit-shared and loaded by
// tests/hot_swap/harness.cpp, which swaps it for v2.js in the same process.
//
// The module assigns its API onto globalThis — the SAME names v2 assigns, so
// the swap overwrites them — and parks ~24MB of typed arrays in module scope.
// The size is the test's real assertion: the heap's semispace is 32MB
// (rt_state.cpp reserves 64MB total), so v1's payload and v2's cannot both be
// live across the swap. The run only survives when unloadModule really
// dropped v1's spans and v1's payload died under v2's allocations.
let payload = [];

function version() {
  return "v1";
}

function checksum() {
  let sum = 0;
  for (let i = 0; i < payload.length; i++) {
    const v = payload[i];
    sum += v[0] + v[v.length - 1];
  }
  return sum;
}

// A small plain object with no closure behind it: the harness roots one in a
// Persistent across the swap, proving unload kills the module's ROOTS and not
// the module's surviving VALUES.
function makeToken() {
  return { tag: "v1", n: 42 };
}

globalThis.hotVersion = version;
globalThis.hotChecksum = checksum;
globalThis.hotToken = makeToken;

for (let i = 0; i < 3; i++) {
  const v = new Float64Array(1024 * 1024);
  v[0] = (i + 1) * 100 + 1;
  v[v.length - 1] = 1;
  payload.push(v);
}
console.log("v1: ready checksum=" + checksum());
