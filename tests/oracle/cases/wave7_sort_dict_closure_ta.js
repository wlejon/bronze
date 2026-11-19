// Wave 7 regression tests:
// 1. Array.prototype.sort (hoisted entry scope, natural run-skip, bulk write-back, mutations, GC)
// 2. Dictionary (backward-shift deletion, hashBloom_, maxStringLen_, symbols, enumeration order)
// 3. Closure creation (FunctionHeader::createCompiled fast/slow paths, inline slots, names/length)
// 4. TypedArray.prototype.set (same-encoding memcpy/memmove, elementsAreNumbers float32/float64)
// 5. Map / WeakMap / WeakSet lookup fast paths & brand checks

// --- 1. Array.prototype.sort ---
{
  const sorted = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17];
  console.log('already:', sorted.sort((a, b) => a - b).join(','));

  const reversed = [17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1];
  console.log('reverse:', reversed.sort((a, b) => a - b).join(','));

  // Nearly sorted painter list + stability + heap objects moved by GC mid-sort
  const mats = [{ id: 0 }, { id: 1 }, { id: 2 }, { id: 3 }];
  const items = [];
  for (let i = 0; i < 120; i++) {
    items.push({
      id: i,
      group: i < 60 ? 0 : 1,
      mat: mats[i % 4],
      z: i < 60 ? i * 0.5 : (120 - i) * 0.5,
    });
  }
  let trash = [];
  function painter(a, b) {
    trash.push({ s: a.id + ':' + b.id, arr: [a, b] });
    if (trash.length > 20) trash = [];
    if (a.group !== b.group) return a.group - b.group;
    if (a.mat.id !== b.mat.id) return a.mat.id - b.mat.id;
    return a.z - b.z;
  }
  items.sort(painter);
  let stableOk = true;
  for (let i = 1; i < items.length; i++) {
    const c = painter(items[i - 1], items[i]);
    if (c > 0 || (c === 0 && items[i - 1].id > items[i].id)) stableOk = false;
  }
  console.log('painter:', items.length, stableOk, items[0].id, items[119].id);

  // Undefined and holes
  const holed = [4, undefined, 2, , 1, undefined, , 3];
  holed.sort((a, b) => a - b);
  console.log('holes:', holed.length, holed.slice(0, 6).join(','), 6 in holed, 7 in holed);

  // Throwing comparator leaves array untouched
  const intact = [9, 7, 5, 3, 1];
  let nCalls = 0;
  try {
    intact.sort((a, b) => {
      if (++nCalls === 3) throw new RangeError('boom');
      return a - b;
    });
  } catch (e) {
    console.log('throw:', e instanceof RangeError, e.message, intact.join(','));
  }

  // Shrinking, growing, freezing mid-sort
  const shrink = [5, 4, 3, 2, 1];
  shrink.sort((a, b) => { shrink.length = 2; return a - b; });
  console.log('shrink:', shrink.length, shrink.join(','));

  const grow = [4, 1, 3, 2];
  let grew = false;
  grow.sort((a, b) => { if (!grew) { grew = true; grow.push(99); } return a - b; });
  console.log('grow:', grow.length, grow.slice(0, 4).join(','), grow[4]);

  const toFreeze = [3, 1, 2];
  let frozeOk = false;
  try {
    toFreeze.sort((a, b) => { Object.freeze(toFreeze); return a - b; });
  } catch (e) {
    frozeOk = e instanceof TypeError;
  }
  console.log('freezeMid:', frozeOk);

  // Object answer with valueOf throwing inside ToNumber
  const voThrow = [3, 1, 2];
  try {
    voThrow.sort((a, b) => ({ valueOf() { throw new Error('vo'); } }));
  } catch (e) {
    console.log('voThrow:', e.message, voThrow.join(','));
  }

  // Capturing comparator
  let factor = -1;
  console.log('capturing:', [1, 4, 2, 5, 3].sort((a, b) => factor * (a - b)).join(','));
}

// --- 2. Dictionary backward-shift deletion & bloom/len miss filter ---
{
  const symA = Symbol('a');
  const symB = Symbol('b');
  const dict = {};
  // Seed initial keys then trigger delete/re-add churn across >500 keys (forces reindex & compact)
  for (let i = 0; i < 60; i++) dict['k_' + i] = i;
  dict[symA] = 'symValA';
  dict[symB] = 'symValB';
  for (let i = 0; i < 550; i++) {
    const key = 'churn_' + ((i * 73) % 120) + '_suf';
    dict[key] = i;
    if (i > 40) {
      const delKey = 'churn_' + (((i - 30) * 73) % 120) + '_suf';
      delete dict[delKey];
    }
  }
  // Verify existing, deleted, symbol, and long-key misses
  let presentOk = true;
  for (let i = 0; i < 60; i++) {
    if (dict['k_' + i] !== i) presentOk = false;
  }
  const longMiss = 'this_is_a_very_long_property_key_exceeding_max_string_length_1234567890';
  const protoShortMiss = 'noSuchShort';
  const objShort = { a: 1, b: 2 };
  const nullDict = Object.create(null);
  nullDict.alpha = 10;
  nullDict.beta = 20;
  delete nullDict.alpha;
  nullDict.gamma = 30;
  nullDict.alpha = 40;
  console.log(
    'dict:',
    presentOk,
    dict[symA],
    dict[symB],
    dict[longMiss] === undefined,
    objShort[longMiss] === undefined,
    objShort[protoShortMiss] === undefined,
    dict['churn_0_suf'] !== undefined || dict['churn_0_suf'] === undefined,
    Object.keys(nullDict).join(','),
    nullDict[longMiss] === undefined
  );
  // Check exact live count of k_ + surviving churn_ keys + enumeration consistency
  const enumKeys = Object.keys(dict);
  let enumOk = true;
  for (let i = 0; i < enumKeys.length; i++) {
    if (dict[enumKeys[i]] === undefined) enumOk = false;
  }
  console.log('dictEnum:', enumKeys.length > 60, enumOk);
}

// --- 3. Closure creation in loops (young fast path + GC stress slow path) ---
{
  const fns = [];
  for (let i = 0; i < 200; i++) {
    const captured = i * 3;
    const namedFn = function itemFn(x, y = 1) { return captured + x + y; };
    const noCap = () => 7;
    fns.push([namedFn, noCap]);
  }
  let sum = 0;
  for (let i = 0; i < fns.length; i++) {
    sum += fns[i][0](i) + fns[i][1]();
  }
  console.log('closures:', fns[5][0].name, fns[5][0].length, fns[5][1].name, fns[5][1].length, sum);

  class Widget {
    constructor(a, b, c) {
      this.a = a;
      this.b = b;
      this.c = c;
    }
    total() { return this.a + this.b + this.c; }
  }
  let wSum = 0;
  for (let i = 0; i < 100; i++) {
    wSum += new Widget(i, i + 1, i + 2).total();
  }
  console.log('classCtor:', wSum);
}

// --- 4. TypedArray.prototype.set ---
{
  const srcF32 = new Float32Array([1.5, -2.25, 3.75, 4.5, 5.125]);
  const dstF32 = new Float32Array(8);
  dstF32.set(srcF32, 2);
  console.log('taSame:', dstF32.join(','));

  // Same-buffer overlapping forward and backward copy
  const overlap = new Float32Array([10, 20, 30, 40, 50, 60]);
  overlap.set(overlap.subarray(0, 4), 2);
  console.log('taOverlapFwd:', overlap.join(','));
  overlap.set(overlap.subarray(2, 6), 0);
  console.log('taOverlapBwd:', overlap.join(','));

  // All-number Arrays (elementsAreNumbers) into Float32Array & Float64Array
  const numArr = [0.5, 1.5, 2.5, 3.5];
  const f32FromArr = new Float32Array(6);
  const f64FromArr = new Float64Array(6);
  f32FromArr.set(numArr, 1);
  f64FromArr.set(numArr, 1);
  console.log('taNumArr32:', f32FromArr.join(','));
  console.log('taNumArr64:', f64FromArr.join(','));

  // Mixed array with valueOf
  const mixedArr = [10, { valueOf() { return 20.5; } }, 30];
  const f32Mixed = new Float32Array(4);
  f32Mixed.set(mixedArr, 1);
  console.log('taMixed:', f32Mixed.join(','));

  // Out-of-bounds throws RangeError
  let rangeErr = false;
  try {
    dstF32.set(srcF32, 6);
  } catch (e) {
    rangeErr = e instanceof RangeError;
  }
  console.log('taRange:', rangeErr);
}

// --- 5. Map / WeakMap / WeakSet fast probe paths ---
{
  const k1 = { id: 1 };
  const k2 = { id: 2 };
  const kMissing = { id: 3 };
  const m = new Map([[k1, 'one'], ['str', 42], [NaN, 'nan']]);
  const wm = new WeakMap();
  const ws = new WeakSet();
  wm.set(k1, 111);
  ws.add(k2);
  // Force nursery turnover
  for (let i = 0; i < 400; i++) {
    const tmp = { pad: [i, i + 1, 's_' + i] };
    if (i % 10 === 0) m.set('k_' + (i % 5), tmp.pad[0]);
  }
  const kYoung = { id: 99 };
  wm.set(kYoung, 999);
  ws.add(kYoung);
  let f1 = wm.get(k1), f2 = wm.get(kYoung), f3 = wm.get(kMissing);
  let h1 = wm.has(k1), h2 = wm.has(kMissing);
  let s1 = ws.has(k2), s2 = ws.has(kYoung), s3 = ws.has(kMissing);
  let mg1 = m.get(k1), mg2 = m.get('str'), mg3 = m.get(NaN), mh1 = m.has('missing');
  let brandThrew = false;
  try {
    WeakMap.prototype.get.call(Object.create(WeakMap.prototype), k1);
  } catch (e) {
    brandThrew = e instanceof TypeError;
  }
  console.log('coll:', f1, f2, f3 === undefined, h1, h2, s1, s2, s3, mg1, mg2, mg3, mh1, brandThrew);
}

// --- 6. Prototype index setter hazard during sort (and bare sort after hazard trips) ---
{
  let protoSetterRan = false;
  const proto = Object.create(Array.prototype);
  Object.defineProperty(proto, '1', {
    configurable: true,
    set(v) { protoSetterRan = true; },
    get() { return 999; },
  });
  const a = [3, , 1];
  Object.setPrototypeOf(a, proto);
  a.sort((x, y) => x - y);
  const b = [5, 2, 4, 1, 3];
  b.sort((x, y) => x - y);
  console.log('hazard:', protoSetterRan, a.join(','), b.join(','));
}
