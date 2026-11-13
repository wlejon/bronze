// An Array element read again with only named reads of that element in between
// is the first read (lower/elem_reread.h); a named read that misses its shape
// reads the element again, because the miss may run a getter that stores one.
// Each function reads `list[i]` more than once; the lists are then given
// elements whose getters replace `list[i]`, elements of other shapes, holes,
// stores and calls between the reads.

function keepOthers(list, fn, context) {
  const out = [];
  for (let i = 0; i < list.length; i++) {
    if (list[i].fn !== fn || (context && list[i].context !== context)) out.push(list[i]);
  }
  return out;
}

function twice(list, i) {
  return list[i].a + ':' + list[i].b;
}

function withCall(list, i, f) {
  const a = list[i].a;
  f(list, i);
  return a + ':' + list[i].a;
}

function withStore(list, i, v) {
  const a = list[i].a;
  list[i] = v;
  return a + ':' + list[i].a;
}

function withOtherStore(list, other, i, v) {
  const a = list[i].a;
  other[i] = v;
  return a + ':' + list[i].a;
}

function branchy(list, i, flag) {
  let s = list[i].a;
  if (flag) s += list[i].b;
  else list[i] = { a: 'new', b: 'B' };
  return s + ':' + list[i].a;
}

function holes(list, i) {
  const a = list[i];
  const b = list[i];
  return (a === undefined) + ':' + (b === undefined) + ':' + (a === b);
}

const f = function () {};
const g = function () {};
const ctx = { name: 'ctx' };
const plain = [];
for (let i = 0; i < 64; i++) plain.push({ fn: i % 4 === 0 ? f : g, context: i % 2 === 0 ? ctx : plain, once: false });
let n = 0;
for (let r = 0; r < 400; r++) n += keepOthers(plain, f, r % 2 === 0 ? ctx : null).length;
console.log('keep', n);

const pairs = [];
for (let i = 0; i < 16; i++) pairs.push({ a: 'a' + i, b: 'b' + i });
let text = '';
for (let r = 0; r < 400; r++) text = twice(pairs, r % 16);
console.log('twice', text);

// A getter on `a` that replaces the element: the second read sees the new one.
const swapping = [];
for (let i = 0; i < 4; i++) {
  swapping.push({
    get a() { swapping[i] = { a: 'after' + i, b: 'afterB' + i }; return 'before' + i; },
    b: 'oldB' + i,
  });
}
for (let i = 0; i < 4; i++) console.log('getter', twice(swapping, i), twice(swapping, i));

// A getter on `fn` in the listener loop: what is pushed is the element as it
// stands after the getter ran.
const tricky = [];
for (let i = 0; i < 4; i++) {
  tricky.push({
    get fn() { tricky[i] = { fn: g, context: ctx, label: 'replaced' + i }; return g; },
    context: ctx,
    label: 'original' + i,
  });
}
console.log('pushed', keepOthers(tricky, f, null).map((e) => e.label).join(' '));
console.log('pushed', keepOthers(tricky, f, ctx).map((e) => e.label).join(' '));

// Other shapes at the same sites (the read's claim misses, nothing changes).
const shapes = [{ b: 'b0', a: 'a0' }, { a: 'a1', b: 'b1', c: 1 }, Object.assign(Object.create({ a: 'pa' }), { b: 'b2' }), { a: 'a3', b: 'b3' }];
for (let i = 0; i < 4; i++) console.log('shape', twice(shapes, i));

// A call, a store and a store to another array between the reads.
const called = [{ a: 'x0' }, { a: 'x1' }];
for (let r = 0; r < 300; r++) withCall(called, r % 2, () => {});
console.log('call', withCall(called, 0, (l, i) => { l[i] = { a: 'y0' }; }), withCall(called, 1, () => {}));
for (let r = 0; r < 300; r++) withStore(called, r % 2, { a: 'z' + (r % 5) });
console.log('store', withStore(called, 0, { a: 'last' }));
const other = [{ a: 'o0' }, { a: 'o1' }];
for (let r = 0; r < 300; r++) withOtherStore(called, other, r % 2, { a: 'w' });
console.log('other', withOtherStore(called, other, 0, { a: 'w2' }), withOtherStore(called, called, 0, { a: 'alias' }));

const br = [{ a: 'p', b: 'q' }, { a: 'r', b: 's' }];
for (let r = 0; r < 300; r++) branchy(br, r % 2, true);
console.log('branch', branchy(br, 0, true), branchy(br, 1, false), branchy(br, 1, true));

// Holes and indices past the end.
const holey = [1, , 3];
holey.length = 5;
for (let r = 0; r < 300; r++) holes(holey, r % 7);
console.log('holes', holes(holey, 0), holes(holey, 1), holes(holey, 4), holes(holey, 9), holes(holey, -1));
