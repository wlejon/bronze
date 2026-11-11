// A literal of spelled data properties has its values evaluated first and is
// made and filled after the last (lower_object.cpp). The order of the value
// expressions, a throw part way, a repeated key, a method, a yield between
// two values and a value that is itself a literal must all read as before.

const log = [];
const src = {
  get a() { log.push('a'); return 1; },
  get b() { log.push('b'); return 2; },
  f(x) { log.push('f' + x); return x * 10; },
};
const o = { a: src.a, n: src.f(3), b: src.b, m() { return this.n; }, a2: src.a ? { deep: src.f(4) } : null };
console.log(JSON.stringify(o), o.m(), Object.keys(o).join(), log.join());

const dup = { k: src.f(1), k: src.f(2), z: 0 };
console.log(JSON.stringify(dup), Object.keys(dup).join());

try {
  const t = { x: src.f(5), y: (() => { throw new Error('mid'); })(), z: src.f(6) };
  console.log(t);
} catch (e) {
  console.log(e.message, log.join());
}

function* g() { return { p: yield 1, q: yield 2, r: 3 }; }
const it = g();
it.next();
it.next('P');
console.log(JSON.stringify(it.next('Q').value));
