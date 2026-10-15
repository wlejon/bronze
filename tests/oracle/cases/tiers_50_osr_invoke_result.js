// An inner loop OSR-entered under a for-of whose body calls a function and
// then reads its result: the loop's value (the call's result) and the for-of
// binding are live into the inner loop header and defined again on every
// outer pass. The OSR code must carry each pass's own values, not the ones
// the frame entered it with. The call lowers to an invoke (the for-of closes
// its iterator on a throw), so its result exists only on the invoke's normal
// edge. Every call must return the same vector on every tier.
const D = 512;
function wv(seed) {
  const v = new Float32Array(D);
  let a = seed;
  for (let i = 0; i < D; i++) {
    a = (Math.imul(a, 1664525) + 1013904223) >>> 0;
    v[i] = a / 4294967296 - 0.5;
  }
  return v;
}

function key(v) {
  let s = 0;
  for (let i = 0; i < v.length; i++) s += v[i] * (i + 1);
  return s;
}

function lastAssign(ws) {
  const v = new Float32Array(D);
  for (const w of ws) {
    const x = wv(w);
    for (let i = 0; i < D; i++) v[i] = x[i];
  }
  return v;
}

function sumF64(ws) {
  const v = new Float64Array(D);
  for (const w of ws) {
    const x = wv(w);
    for (let i = 0; i < D; i++) v[i] += x[i];
  }
  return v;
}

function lastSeed(ws) {
  const v = new Float64Array(D);
  for (const w of ws) {
    const x = wv(w);
    for (let i = 0; i < D; i++) v[i] = x[i];
    v[0] = w;
  }
  return v;
}

const words = [11, 22, 33, 44, 55, 66];
for (const [name, f] of [['lastAssign', lastAssign], ['sumF64', sumF64], ['lastSeed', lastSeed]]) {
  const first = key(f(words));
  let differ = 0;
  let seed = 0;
  for (let r = 0; r < 400; r++) {
    const v = f(words);
    if (key(v) !== first) differ++;
    seed = v[0];
  }
  console.log(name + ': ' + differ + ' differ, key ' + first.toFixed(6) + ', v[0] ' + seed);
}
