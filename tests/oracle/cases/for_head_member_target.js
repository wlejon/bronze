// A for-in or for-of head with no declaration keyword ASSIGNS on every
// iteration (ECMA-262 14.7.5.7, lhsKind assignment), and what it assigns to may
// be a property reference, or a pattern holding one, as well as a name.

const o = {};
for (o.a of [1, 2]) {}
for ([o.b, , o['c']] of [[3, 0, 4]]) {}
for ({ x: o.d, y: [o.e] = [5] } of [{ x: 6 }]) {}
for ({ z: o.f = 'dflt', ...o.rest } of [{ u: 1 }]) {}
for (o.key in { p: 1, q: 2 }) {}
for ([o.first] in { ab: 1 }) {}
console.log(JSON.stringify(o));

// The reference is evaluated each iteration, after the next value is taken.
const log = [];
const box = { i: 0 };
function slot() {
  log.push('slot' + box.i);
  return 'k' + box.i++;
}
function* values() {
  log.push('next');
  yield 'a';
  log.push('next');
  yield 'b';
}
const dst = {};
for (dst[slot()] of values()) log.push('body');
console.log(JSON.stringify(dst), log.join(' '));

// Name-only heads still assign, including a parenthesized name, and a name
// spelled `of`.
let a, b;
for ([a, b] of [[7, 8]]) {}
for ((a) of [9]) {}
var of;
for (of of [10]) {}
console.log(a, b, of);

// The body's own `let o` does not shadow the head, and `continue` with a
// label still reaches the loop.
const seen = [];
outer: for (o.g of [1, 2, 3]) {
  let o = 'inner';
  for (;;) {
    if (seen.length === 1) {
      seen.push('skip');
      continue outer;
    }
    break;
  }
  seen.push(o);
}
console.log(o.g, seen.join(','));

// In a generator the target may suspend; in an async function it may await,
// and `for await` assigns through a reference too.
function* gen() {
  const t = {};
  for (t[yield 'key?'] of [1, 2]) {}
  return t;
}
const it = gen();
const got = [it.next().value, it.next('one').value];
console.log(got.join(','), JSON.stringify(it.next('two').value));

async function* agen() {
  yield 'x';
  yield 'y';
}
async function viaAwait(p) {
  const t = {};
  for (t[await p] of [3]) {}
  for await (t.last of agen()) {}
  for await ([t.arr] of agen()) {}
  return t;
}
viaAwait(Promise.resolve('w')).then((t) => console.log(JSON.stringify(t)));
