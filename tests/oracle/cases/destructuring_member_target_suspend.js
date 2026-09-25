// A member target of a destructuring assignment is evaluated code: its object
// and computed key may suspend (`yield`, `await`), may call, and may close over
// the enclosing scope. Each of those has to be seen by every walk over the
// tree, not only by the one that lowers the assignment.

function* gen() {
  const t = {};
  [t[yield 'key?']] = [7];
  ({ v: t[yield 'key2?'] = 8 } = {});
  return t;
}
const it = gen();
console.log(it.next().value);
console.log(it.next('first').value);
console.log(JSON.stringify(it.next('second').value));

async function viaAwait(p) {
  const t = {};
  [t[await p], ...t[await p + 'Rest']] = [1, 2, 3];
  ({ x: t.y = await p } = {});
  return t;
}
viaAwait(Promise.resolve('w')).then((t) => console.log(JSON.stringify(t)));

// A call site that exists only inside a target.
function keyFor(n) {
  return 'k' + n;
}
console.log(keyFor(1));
const h = {};
[h[keyFor('x')]] = [5];
({ a: h[keyFor('y')] } = { a: 6 });
console.log(JSON.stringify(h));

// A closure whose only mention of an outer binding is a target's object.
function make() {
  const o = {};
  const k = 'a';
  const set = (src) => {
    ({ v: o[k] } = src);
    [o.second, ...o.rest] = [1, 2, 3];
  };
  return { o, set };
}
const m = make();
m.set({ v: 42 });
console.log(JSON.stringify(m.o));

// A nested pattern with a default, in both literal shapes.
let x, y, z;
[[x] = [99]] = [];
({ a: { b: y } = { b: 98 } } = {});
[{ c: z } = { c: 97 }] = [undefined];
console.log(x, y, z);
