// Hot-swap module, version 2 — v1.js's replacement, same globalThis names,
// same ~24MB module-scope payload, different values. v1.js states the sizing
// argument that makes the payload an assertion rather than ballast.
//
// The globalThis assignments run BEFORE the payload allocation on purpose:
// overwriting v1's names is what drops the last references into v1's module
// environment, so by the time this version's arrays force a collection, v1's
// payload is garbage — provided the host really unloaded v1's spans.
let payload = [];

function version() {
  return "v2";
}

function checksum() {
  let sum = 0;
  for (let i = 0; i < payload.length; i++) {
    const v = payload[i];
    sum += v[0] + v[v.length - 1];
  }
  return sum;
}

function makeToken() {
  return { tag: "v2", n: 43 };
}

globalThis.hotVersion = version;
globalThis.hotChecksum = checksum;
globalThis.hotToken = makeToken;

for (let i = 0; i < 3; i++) {
  const v = new Float64Array(1024 * 1024);
  v[0] = (i + 1) * 1000 + 2;
  v[v.length - 1] = 1;
  payload.push(v);
}
console.log("v2: ready checksum=" + checksum());
