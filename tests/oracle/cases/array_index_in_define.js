// `k in O` and element definition with Number keys on arrays: own elements,
// holes, indices past the end, -0, fractions, frozen and non-extensible
// arrays, Array subclasses, computed numeric keys on classes and literals.
class Sp extends Array {}
try { const o = { __proto__: null }; const r = Object.fromEntries([[0, 1]]); console.log(JSON.stringify(r)); } catch (e) { console.log('E', e.message); }
const n = Object.freeze([1, 2]);
console.log(n.map(x => x * 2).join(), [3, 4].filter(x => x > 3).join(), Array.from({ length: 3 }, (_, i) => i).join());
class D { static [0] = 1; } console.log(D[0]);
const f2 = [9]; Object.preventExtensions(f2);
try { Object.defineProperty(f2, 1, { value: 1 }); } catch (e) { console.log('TE'); }
const sp = Sp.from([1, 2, 3]).map(x => x + 1); console.log(sp instanceof Sp, sp.join());
const c = { [2]: 'two', [1.5]: 'x' }; console.log(JSON.stringify(c));
const h = [1, , 3]; console.log(0 in h, 1 in h, 2 in h, 3 in h, -0 in h, 0.5 in h, 4294967295 in h, h.map(x => x * 2).length, 1 in h.map(x => x));
