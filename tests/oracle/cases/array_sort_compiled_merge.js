// `arr.sort(comparefn)` at a site in compiled code: an image build runs the
// merge in the module's stub (il2mir/il_array_sort.cpp) for an Array with no
// properties object, and the builtin for everything else. Every sort below is
// inside a function called more than once, which is where the site's arm is
// emitted; each result is printed once. What is pinned: stability, undefined
// last and holes after it, a comparator that throws (the array untouched), one
// that mutates the array (the write-back of the snapshot wins), a comparator
// that is no function, receivers that are not plain Arrays, and the three.js
// render-list comparators. The stub must also call the comparator with the
// same pairs in the same order as the builtin reached by `.call`.

function show(label, value, pass) {
  if (pass === 0) console.log(label, value);
}

// --- 1. three.js painter sorts over a render list, several frames ------------
function painterSortStable(a, b) {
  if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
  else if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
  else if (a.material.id !== b.material.id) return a.material.id - b.material.id;
  else if (a.z !== b.z) return a.z - b.z;
  else return a.id - b.id;
}
function reversePainterSortStable(a, b) {
  if (a.groupOrder !== b.groupOrder) return a.groupOrder - b.groupOrder;
  else if (a.renderOrder !== b.renderOrder) return a.renderOrder - b.renderOrder;
  else if (a.z !== b.z) return b.z - a.z;
  else return a.id - b.id;
}
const mats = [];
for (let i = 0; i < 5; i++) mats.push(i % 2 ? { id: i, kind: 'a' } : { name: 'm', id: i });
let seed = 4242;
function rnd() { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; }
const items = [];
for (let i = 0; i < 700; i++) {
  items.push({ id: i, object: null, material: mats[i % 5], groupOrder: i % 11 === 0 ? 1 : 0,
               renderOrder: 0, z: Math.floor(rnd() * 40), group: null });
}
const customOpaqueSort = null;
function renderList(frame) {
  const opaque = [], transparent = [];
  for (let i = 0; i < items.length; i++) {
    items[i].z = Math.floor(rnd() * 40);
    (items[i].id % 4 === 0 ? transparent : opaque).push(items[i]);
  }
  if (opaque.length > 1) opaque.sort(customOpaqueSort || painterSortStable);
  if (transparent.length > 1) transparent.sort(customOpaqueSort || reversePainterSortStable);
  let ok = true;
  for (let i = 1; i < opaque.length; i++) if (painterSortStable(opaque[i - 1], opaque[i]) > 0) ok = false;
  for (let i = 1; i < transparent.length; i++) {
    if (reversePainterSortStable(transparent[i - 1], transparent[i]) > 0) ok = false;
  }
  return ok + ' ' + opaque.length + ' ' + transparent.length + ' ' + opaque[0].id + ' ' + transparent[0].id;
}
for (let f = 0; f < 4; f++) console.log('frame', f, renderList(f));

// --- 2. stability: equal keys keep their input order -------------------------
function stability(pass) {
  const xs = [];
  for (let i = 0; i < 300; i++) xs.push({ k: (i * 37) % 7, i });
  xs.sort((a, b) => a.k - b.k);
  let ok = true;
  for (let i = 1; i < xs.length; i++) {
    if (xs[i - 1].k > xs[i].k || (xs[i - 1].k === xs[i].k && xs[i - 1].i > xs[i].i)) ok = false;
  }
  show('stable', ok + ' ' + xs[0].i + ' ' + xs[1].i + ' ' + xs[299].i, pass);
}
for (let p = 0; p < 2; p++) stability(p);

// --- 3. undefined sorts last without a call; holes go after it --------------
function undefinedsAndHoles(pass) {
  const seen = [];
  const a = [5, undefined, 3, , 1, undefined, , 4, 2];
  a.sort((x, y) => { seen.push(x === undefined || y === undefined); return x - y; });
  show('holes', a.length + ' ' + JSON.stringify(a) + ' ' + (7 in a) + ' ' + (8 in a) + ' ' + seen.includes(true), pass);
  const allUndef = [undefined, undefined, undefined];
  let calls = 0;
  allUndef.sort(() => { calls++; return 0; });
  show('all-undefined', allUndef.length + ' ' + calls, pass);
}
for (let p = 0; p < 2; p++) undefinedsAndHoles(p);

// --- 4. a comparator that throws: the array is untouched ---------------------
function throwing(pass) {
  const a = [9, 8, 7, 6, 5, 4, 3, 2, 1];
  let n = 0;
  try {
    a.sort((x, y) => { if (++n === 6) throw new RangeError('cmp ' + n); return x - y; });
  } catch (e) {
    show('threw', e.name + ' ' + e.message + ' ' + a.join(','), pass);
  }
  try {
    a.sort((x, y) => ({ valueOf() { throw new Error('valueOf'); } }));
  } catch (e) {
    show('threw-valueOf', e.message + ' ' + a.join(','), pass);
  }
  try {
    a.sort(() => 1n);
  } catch (e) {
    show('threw-bigint', (e instanceof TypeError) + ' ' + a.join(','), pass);
  }
}
for (let p = 0; p < 2; p++) throwing(p);

// --- 5. a comparator that mutates the array ----------------------------------
function mutating(pass) {
  const a = [5, 4, 3, 2, 1];
  a.sort((x, y) => { a.length = 1; return x - y; });
  show('shrink', a.length + ' ' + a.join(','), pass);
  const b = [3, 2, 1];
  let pushed = false;
  b.sort((x, y) => { if (!pushed) { pushed = true; b.push(0); } return x - y; });
  show('grow', b.length + ' ' + b.join(','), pass);
  const c = [4, 1, 3, 2];
  c.sort((x, y) => { c[0] = 99; c[3] = -1; return x - y; });
  show('overwrite', c.join(','), pass);
}
for (let p = 0; p < 2; p++) mutating(p);

// --- 6. comparators that are not functions; answers that are not numbers ----
function notFunctions(pass) {
  for (const bad of [null, 1, 'x', {}, []]) {
    const a = [2, 1];
    try { a.sort(bad); show('bad', 'no throw', pass); } catch (e) { show('bad', (e instanceof TypeError) + ' ' + a.join(','), pass); }
  }
  const d = [10, 9, 1];
  d.sort(undefined);
  show('default', d.join(','), pass);
  show('nan', [3, 1, 2].sort(() => NaN).join(','), pass);
  show('string', [3, 1, 2].sort((x, y) => String(x - y)).join(','), pass);
  show('object', [3, 1, 2].sort((x, y) => ({ valueOf() { return y - x; } })).join(','), pass);
  const bound = function (x, y) { return this.dir * (x - y); }.bind({ dir: -1 });
  show('bound', [5, 1, 4, 2].sort(bound).join(','), pass);
  show('proxy', [5, 1, 4, 2].sort(new Proxy((x, y) => x - y, {})).join(','), pass);
  let classThrew = false;
  try { [2, 1].sort(class {}); } catch (e) { classThrew = e instanceof TypeError; }
  show('class', classThrew, pass);
}
for (let p = 0; p < 2; p++) notFunctions(p);

// --- 7. receivers that are not plain Arrays ----------------------------------
class Sub extends Array {}
function receivers(pass) {
  const like = { length: 4, 0: 'd', 1: 'b', 3: 'a', sort: Array.prototype.sort };
  like.sort((x, y) => (x < y ? -1 : x > y ? 1 : 0));
  show('array-like', JSON.stringify(like) + ' ' + (3 in like), pass);
  const own = [3, 1, 2];
  own.sort = function (f) { return 'own ' + f(1, 2); };
  show('own-sort', own.sort((x, y) => x - y), pass);
  const s = Sub.from([3, 1, 2]);
  const r = s.sort((x, y) => x - y);
  show('subclass', (r === s) + ' ' + (r instanceof Sub) + ' ' + r.join(','), pass);
  const ta = new Int16Array([3, -1, 2]);
  show('typed', ta.sort((x, y) => y - x).join(','), pass);
  const frozen = Object.freeze([2, 1]);
  try { frozen.sort((x, y) => x - y); show('frozen', 'no throw', pass); } catch (e) { show('frozen', e instanceof TypeError, pass); }
  const custom = { sort(f) { return 'custom ' + f(2, 1); } };
  show('custom', custom.sort((x, y) => x - y), pass);
  const viaProto = [3, 1, 2];
  Object.setPrototypeOf(viaProto, { sort() { return 'proto'; } });
  show('proto', viaProto.sort((x, y) => x - y), pass);
}
for (let p = 0; p < 2; p++) receivers(p);

// --- 8. the stub asks the comparator what the builtin asks, in order ---------
function sameCalls(pass) {
  const base = [];
  for (let i = 0; i < 37; i++) base.push((i * 13) % 17);
  base.push(undefined, 4, undefined);
  const log1 = [], log2 = [];
  const a = base.slice(), b = base.slice();
  a.sort((x, y) => { log1.push(x + ':' + y); return x - y; });
  Array.prototype.sort.call(b, (x, y) => { log2.push(x + ':' + y); return x - y; });
  show('same-calls', (log1.join() === log2.join()) + ' ' + (a.join() === b.join()) + ' ' + a.join(','), pass);
}
for (let p = 0; p < 2; p++) sameCalls(p);

// --- 9. a comparator that collects: the moved Values are the ones written ----
function collecting(pass) {
  const xs = [];
  for (let i = 0; i < 400; i++) xs.push({ k: 'key' + ((i * 7919) % 400), n: i });
  let junk = [];
  xs.sort((a, b) => {
    junk.push({ s: a.k + b.k, arr: [a.n, b.n, a, b] });
    if (junk.length > 50) junk = [];
    return a.k < b.k ? -1 : a.k > b.k ? 1 : 0;
  });
  let ok = true;
  for (let i = 1; i < xs.length; i++) if (xs[i - 1].k >= xs[i].k) ok = false;
  show('collect', ok + ' ' + xs[0].k + ' ' + xs[0].n + ' ' + xs[399].k, pass);
}
for (let p = 0; p < 2; p++) collecting(p);
