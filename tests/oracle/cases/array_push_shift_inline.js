// `push` and `shift` inline at sites whose receiver inference typed an Array
// (il_array_method.cpp), handed arrays the inline arm must not answer for.
//
// The arm appends or takes the front only for an Array with no named-property
// box and, for push, room in its block; everything else goes through the
// site's call to the full algorithm. Each case warms the sites, then gives
// them an array that is frozen, sealed, non-extensible, holey, carrying an own
// `push`, of a subclass, or full. Every line must read as the ordinary call.

const N = 2000;

function pushAll(a, n) {
  for (let i = 0; i < n; i++) a.push(i);
  return a.length;
}
function pushOne(a, v) { return a.push(v); }
function drain(a) {
  let s = 0;
  while (a.length > 0) {
    const v = a.shift();
    s += v === undefined ? 1000 : v;
  }
  return s;
}
function shiftOne(a) { return a.shift(); }
function kind(f) {
  try {
    return String(f());
  } catch (err) {
    return err.constructor.name;
  }
}

// Warm: grown from empty past every capacity step, then emptied from the
// front, then reused.
const a = [];
console.log('warm', pushAll(a, N), drain(a), a.length);
console.log('reuse', pushAll(a, 3), a.join(','), String(shiftOne([])));

// A queue whose front moves while its back grows.
function queue() {
  const q = [];
  let s = 0;
  for (let i = 0; i < N; i++) {
    q.push(i);
    q.push(i);
    s += q.shift();
  }
  return s + ':' + q.length + ':' + q[0] + ':' + q[q.length - 1];
}
console.log('queue', queue());

// References stored by push and taken by shift, across collections.
function objects() {
  const q = [];
  let s = 0;
  for (let i = 0; i < N; i++) q.push({ i, tag: 'o' + i });
  for (let i = 0; i < N; i++) q.push({ i: q.shift().i * 2 });
  for (let i = 0; i < q.length; i++) s += q[i].i;
  return s + ':' + q.length;
}
console.log('objects', objects());

// 1. Integrity levels.
const f = Object.freeze([1, 2]);
console.log('frozen', kind(() => pushOne(f, 3)), kind(() => shiftOne(f)), f.join(','));
const se = Object.seal([1, 2]);
console.log('sealed push', kind(() => pushOne(se, 3)), se.join(','));
const ne = Object.preventExtensions([1, 2]);
console.log('non-extensible push', kind(() => pushOne(ne, 3)), ne.join(','));
console.log('non-extensible shift', kind(() => shiftOne(ne)), ne.join(','), ne.length);

// 2. Holes: a hole shifts out as undefined, and pushing after a hole keeps it.
const h = [1, , 3];
console.log('holey', drain(h), h.length);
const h2 = [, 5];
pushOne(h2, 6);
console.log('holey push', h2.length, 0 in h2, String(shiftOne(h2)), h2.join(','));

// 3. An own `push`, a subclass's, and an array with a named property.
const own = [];
own.push = function (v) { return 'own' + v; };
console.log('own push', pushOne(own, 2), own.length);
class Stack extends Array {
  push(v) { return super.push(v * 10); }
}
const st = new Stack();
console.log('subclass push', pushOne(st, 2), st[0], st.length);
const named = [1, 2];
named.label = 'n';
console.log('named', pushOne(named, 3), shiftOne(named), named.join(','), named.label);
