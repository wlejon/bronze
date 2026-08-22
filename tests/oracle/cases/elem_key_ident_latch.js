// The string-key identity latch of the computed-read cache (elem_ic.h's
// `key_ident`, the inline arm in llvm_elem_cache.cpp): every guard story the
// one-compare mechanism stands on, pinned byte for byte. The case must answer
// identically with BRONZE_NO_ELEM_KEY_IC=1, with every other seam off, and
// under BRONZE_GC_STRESS=1 — the ident sweep is the soundness story, and
// stress makes every loop below cross collections.

// --- 1. the latch itself: one key object, presented repeatedly --------------
const attrs = { position: 10, normal: 20, uv: 30 };
const keyPos = 'position';
let sum = 0;
for (let i = 0; i < 2000; i++) sum += attrs[keyPos];
console.log(sum);

// --- 2. equal-but-distinct key strings --------------------------------------
// A fresh, content-equal string object must answer exactly what the latched
// one does — the helper confirms by content and re-latches, and alternating
// the two objects thrashes the latch in both directions without ever changing
// the answer.
function makeKey(a, b) { return a + b; }
const keyPos2 = makeKey('posi', 'tion');
console.log(keyPos === keyPos2);
let alt = 0;
for (let i = 0; i < 1000; i++) alt += attrs[(i & 1) ? keyPos : keyPos2];
console.log(alt);

// --- 3. shape change after latch --------------------------------------------
// An own add transitions the receiver's shape, so a latched entry may not
// answer for the new shape without re-proving the slot.
const grow = { a: 1, b: 2 };
const keyB = 'b';
let before = 0;
for (let i = 0; i < 100; i++) before += grow[keyB];
grow.c = 7;          // shape transition
grow.b = 5;          // same slot, new value
let after = 0;
for (let i = 0; i < 100; i++) after += grow[keyB];
console.log(before, after);

// --- 4. prototype hit, then shadowing ---------------------------------------
const proto = { speed: 3 };
const child = Object.create(proto);
child.own = 1;
const keySpeed = 'speed';
let viaProto = 0;
for (let i = 0; i < 200; i++) viaProto += child[keySpeed];
child.speed = 100;   // shadow: own property now wins
let shadowed = 0;
for (let i = 0; i < 200; i++) shadowed += child[keySpeed];
proto.speed = 4;     // and the proto write must NOT show through the shadow
console.log(viaProto, shadowed, child[keySpeed], proto[keySpeed]);

// --- 5. absent stays absent, then appears ------------------------------------
const sparse = { present: 1 };
const keyMissing = 'missing';
let undefCount = 0;
for (let i = 0; i < 300; i++) if (sparse[keyMissing] === undefined) undefCount++;
sparse.missing = 42; // the absent entry must die with the shape transition
console.log(undefCount, sparse[keyMissing]);

// --- 6. delete after latch ----------------------------------------------------
const dying = { doomed: 9, kept: 4 };
const keyDoomed = 'doomed';
let preDelete = 0;
for (let i = 0; i < 100; i++) preDelete += dying[keyDoomed];
delete dying.doomed;
console.log(preDelete, dying[keyDoomed], dying.kept);

// --- 7. mixed key kinds at ONE site -------------------------------------------
const mixed = { '7': 'seven', 'true': 'yes', name: 'n' };
const keys = [7, 'name', true, '7', 'true'];
const seen = [];
for (let round = 0; round < 50; round++) {
  for (const k of keys) seen.push(mixed[k]);
}
console.log(seen.length, seen[0], seen[1], seen[2], seen[3], seen[4]);

// --- 8. for-in keys driving computed reads (the three.js pattern) -------------
// Enumeration hands out the shape's arena keys, so these are the reads the
// latch exists for; the sums must not care where the key object came from.
const geometry = { position: 2, normal: 3, uv: 5 };
let product = 1;
for (let i = 0; i < 200; i++) {
  for (const name in geometry) product = (product * geometry[name]) % 1000003;
}
console.log(product);

// Object.keys drives the same sites through an array of the same key objects.
let okSum = 0;
const names = Object.keys(geometry);
for (let i = 0; i < 200; i++) {
  for (let j = 0; j < names.length; j++) okSum += geometry[names[j]];
}
console.log(okSum);

// --- 9. two receivers, one key: shape is half the pair ------------------------
const recvA = { x: 1, y: 2 };
const recvB = { x: 10, y: 20, z: 30 };  // different shape
const keyX = 'x';
let two = 0;
for (let i = 0; i < 400; i++) two += ((i & 1) ? recvA : recvB)[keyX];
console.log(two);

// --- 10. an accessor behind a computed read must RUN every time ---------------
let getterRuns = 0;
const withGetter = {
  get live() { getterRuns++; return getterRuns; }
};
const keyLive = 'live';
let lastLive = 0;
for (let i = 0; i < 150; i++) lastLive = withGetter[keyLive];
console.log(getterRuns, lastLive);

// --- 11. dictionary-mode receiver ---------------------------------------------
// Enough deletes force the object off the shape path entirely; the cache must
// refuse it and the reads must still answer.
const dict = {};
for (let i = 0; i < 40; i++) dict['p' + i] = i;
for (let i = 0; i < 20; i++) delete dict['p' + (i * 2)];
const keyP7 = 'p7';
let dictSum = 0;
for (let i = 0; i < 100; i++) dictSum += dict[keyP7];
console.log(dictSum, dict['p0'], dict['p39']);

// --- 12. non-latin1 keys -------------------------------------------------------
const wide = { 'café': 1, '日本語': 2, 'aé': 3 };
const keyCafe = 'café';
const keyNihongo = '日本語';
let wideSum = 0;
for (let i = 0; i < 300; i++) wideSum += wide[keyCafe] + wide[keyNihongo];
// A content-equal wide key built at runtime:
const keyCafe2 = 'caf' + 'é';
console.log(wideSum, wide[keyCafe2], wide['a' + 'é']);

// --- 13. symbols still take their own path -------------------------------------
const sym = Symbol('s');
const symHolder = { [sym]: 'viaSym', plain: 'viaPlain' };
const keyPlain = 'plain';
let symOk = true;
for (let i = 0; i < 100; i++) {
  const k = (i & 1) ? sym : keyPlain;
  const v = symHolder[k];
  if ((i & 1) && v !== 'viaSym') symOk = false;
  if (!(i & 1) && v !== 'viaPlain') symOk = false;
}
console.log(symOk);

// --- 14. garbage between reads: the latch must survive or re-prove, never lie --
const stable = { alpha: 5, beta: 7 };
const keyAlpha = 'alpha';
let churnSum = 0;
for (let i = 0; i < 500; i++) {
  churnSum += stable[keyAlpha];
  // Movable garbage, including strings content-equal to the key, so a moved
  // or recycled address can never be told apart from a correct one except by
  // the sweep doing its job.
  const noise = ['al' + 'pha', { alpha: -1 }, [i, i + 1]];
  if (noise[1].alpha === 0) console.log('never');
}
console.log(churnSum);
