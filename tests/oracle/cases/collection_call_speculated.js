// `m.get(k)` / `m.has(k)` on a Map, Set, WeakMap or WeakSet at its class's
// instance shape: a harvested site whose one callee was the native becomes
// `is.collection` + `collection.op` (speculate/collection_call.cpp). Every
// receiver off the instance shape leaves to the generic call: a subclass, an
// instance given a property, a non-collection, an object that has the shape
// without the internal slots, and a Set constructed at the Map's shape.
const map = new Map();
const set = new Set();
const weak = new WeakMap();
const weakSet = new WeakSet();
const objs = [];
for (let i = 0; i < 64; i++) {
    const o = { id: i };
    objs.push(o);
    map.set('k' + i, i);
    map.set(i, -i);
    set.add(i * 2);
    if (i % 3 === 0) {
        weak.set(o, i * 10);
        weakSet.add(o);
    }
}
const odd = new Map([[NaN, 'nan'], [-0, 'zero'], [objs[5], 'obj5'], ['k7', 7]]);

function mapGet(m, k) { return m.get(k); }
function mapHas(m, k) { return m.has(k); }
function setHas(s, k) { return s.has(k); }
function weakGet(w, k) { return w.get(k); }
function weakHas(w, k) { return w.has(k); }
// The same calls at sites of their own, for the receivers below: a harvest of
// this run sees those at these sites only, so the sites above stay at one
// receiver shape and are speculated (the adversarial builds send these
// receivers through speculated sites too).
function mapGetAny(m, k) { return m.get(k); }
function mapHasAny(m, k) { return m.has(k); }
function setHasAny(s, k) { return s.has(k); }
function weakGetAny(w, k) { return w.get(k); }
function weakHasAny(w, k) { return w.has(k); }

let acc = 0;
let text = '';
for (let r = 0; r < 600; r++) {
    for (let i = 0; i < 64; i++) {
        acc += mapGet(map, 'k' + i) + mapGet(map, i);
        if (mapHas(map, 'k' + (i + 60))) acc++;
        if (setHas(set, i)) acc += 2;
        const w = weakGet(weak, objs[i]);
        if (w !== undefined) acc += w;
        if (weakHas(weakSet, objs[i])) acc += 3;
        if (weakHas(weak, objs[i])) acc += 5;
    }
}
console.log(acc);

// SameValueZero: NaN, -0 and +0, a string made afresh, an object key.
console.log(mapGetAny(odd, NaN), mapGetAny(odd, 0), mapGetAny(odd, -0), mapGetAny(odd, ['k', 7].join('')),
    mapGetAny(odd, objs[5]), mapGetAny(odd, objs[6]), mapHasAny(odd, 'missing'), setHasAny(set, 4), setHasAny(set, 5));
// Keys a weak collection cannot hold answer as the natives do.
console.log(weakGetAny(weak, 1), weakGetAny(weak, 'x'), weakHasAny(weak, null), weakHasAny(weakSet, Symbol.for('r')));

// Receivers off the instance shape.
class MyMap extends Map { get(k) { return 'sub:' + super.get(k); } }
const sub = new MyMap([['a', 1]]);
const expando = new Map([['a', 2]]);
expando.extra = true;
const own = new Map([['a', 3]]);
const lookalike = { get(k) { return 'plain:' + k; }, has() { return 'plain has'; } };
console.log(mapGetAny(sub, 'a'), mapGetAny(expando, 'a'), mapGetAny(lookalike, 'a'), mapHasAny(lookalike, 'a'));
own.get = function (k) { return 'own:' + k; };
console.log(mapGetAny(own, 'a'), mapHasAny(own, 'a'));
for (const bad of [Object.create(Map.prototype), Reflect.construct(Set, [], Map), Object.create(WeakMap.prototype)]) {
    try {
        console.log('no throw', mapGetAny(bad, 'a'));
    } catch (e) {
        console.log(e instanceof TypeError);
    }
    try {
        console.log('no throw', weakGetAny(bad, objs[0]));
    } catch (e) {
        console.log(e instanceof TypeError);
    }
}
try { mapGetAny(undefined, 1); } catch (e) { console.log('undefined receiver', e instanceof TypeError); }
// A Set's has against a Map, a WeakSet's against a WeakMap, after the sites
// latched on their own class.
console.log(setHasAny(map, 'k1'), weakHasAny(weak, objs[3]), weakHasAny(weakSet, objs[4]));

// Mutation between reads through the same sites, and a collection big enough
// to grow its index while being read.
const grow = new Map();
let seen = 0;
for (let i = 0; i < 3000; i++) {
    grow.set('g' + i, i);
    if (mapGetAny(grow, 'g' + (i >> 1)) === i >> 1) seen++;
    if (i % 7 === 0) grow.delete('g' + (i >> 2));
    if (mapHasAny(grow, 'g' + (i >> 2))) seen++;
}
console.log(seen, grow.size);
