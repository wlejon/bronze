// A function — declared, a class, an arrow, a bound one — is an object, and
// Object.assign, defineProperties, spread, Object.entries, for-in,
// JSON.stringify and object rest treat it as one, as target and as source:
// Potree's `function Lib() {}  Object.assign(Lib, { … })` aborted the
// process. A typed array and a generator object are in the same table. A
// builtin function is left out: bronze's intrinsics are frozen.
function Lib() {}
Object.assign(Lib, { a: 1, b: function () {} });
console.log(Lib.a, typeof Lib.b);
class K { static s = 1; }
const arrow = () => 1;
const bound = function () {}.bind(null);
function* gen() { yield 1; }
const kinds = { Lib: function () {}, K, arrow, bound, gen: gen(), ta: new Uint8Array(2) };
for (const [name, v] of Object.entries(kinds)) {
  const r = [];
  try { Object.assign(v, { a: 1, b: function () {} }); r.push('assign ' + v.a + ' ' + typeof v.b); } catch (e) { r.push('assign ' + e.constructor.name); }
  try { Object.defineProperties(v, { c: { value: 3, enumerable: true } }); r.push('defs ' + v.c); } catch (e) { r.push('defs ' + e.constructor.name); }
  try { r.push('spread ' + JSON.stringify({ ...v })); } catch (e) { r.push('spread ' + e.constructor.name); }
  try { r.push('assignFrom ' + JSON.stringify(Object.assign({}, v))); } catch (e) { r.push('assignFrom ' + e.constructor.name); }
  try { r.push('entries ' + JSON.stringify(Object.entries(v))); } catch (e) { r.push('entries ' + e.constructor.name); }
  try { const ks = []; for (const k in v) ks.push(k); r.push('forin [' + ks + ']'); } catch (e) { r.push('forin ' + e.constructor.name); }
  try { r.push('json ' + JSON.stringify(v)); } catch (e) { r.push('json ' + e.constructor.name); }
  try { const { a, ...rest } = v; r.push('rest ' + JSON.stringify(rest)); } catch (e) { r.push('rest ' + e.constructor.name); }
  console.log(name, r.join(' | '));
}
