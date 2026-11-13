// `push` on a value the function itself made with an array literal, or a
// variable every assignment of which is one (il_lowering_hoist.cpp), skips the
// inline arm's test that the receiver is an Array. What remains is the test for
// a named-property box and for room, so an array that was frozen, sealed, given
// a property or its own `push` after the literal must still take the call.

function collect(list, keep) {
  let out = [];
  for (let i = 0; i < list.length; i++) {
    if (list[i] !== keep) out.push(list[i]);
  }
  return out;
}

function restart(list, n) {
  // The variable is a literal on entry and again on a back edge.
  let out = [];
  let total = 0;
  for (let i = 0; i < list.length; i++) {
    out.push(list[i]);
    if (out.length === n) {
      total += out.length;
      out = [];
    }
  }
  return total * 1000 + out.length;
}

function frozenPush(v) {
  const a = [];
  Object.freeze(a);
  try {
    a.push(v);
  } catch (err) {
    return err.constructor.name + ' ' + a.length;
  }
  return 'pushed ' + a.length;
}

function sealedPush(v) {
  const a = [1];
  Object.seal(a);
  try {
    a.push(v);
  } catch (err) {
    return err.constructor.name + ' ' + a.length;
  }
  return 'pushed ' + a.length;
}

function namedPush(v) {
  const a = [];
  a.tag = 'named';
  const n = a.push(v);
  return n + ' ' + a.tag + ' ' + a.length;
}

function ownPush(v) {
  const a = [];
  a.push = function (x) { return 'own ' + x; };
  return a.push(v) + ' ' + a.length;
}

function mixed(flag, v) {
  // One arm is not a literal: the receiver is tested as before.
  let a = [];
  if (flag) a = { push(x) { return 'object ' + x; } };
  return String(a.push(v));
}

function grow(n) {
  const a = [];
  for (let i = 0; i < n; i++) a.push(i * 2);
  let s = 0;
  for (let i = 0; i < a.length; i++) s += a[i];
  return a.length + ' ' + s;
}

const list = [];
for (let i = 0; i < 100; i++) list.push({ id: i });
let kept = 0;
for (let r = 0; r < 300; r++) kept += collect(list, list[r % 100]).length;
console.log('collect', kept);
console.log('restart', restart(list, 7), restart(list, 100), restart(list, 1));
for (let r = 0; r < 300; r++) {
  frozenPush(r);
  sealedPush(r);
  namedPush(r);
  ownPush(r);
  mixed(r % 2 === 0, r);
}
console.log('frozen', frozenPush(1));
console.log('sealed', sealedPush(2));
console.log('named', namedPush(3));
console.log('own', ownPush(4));
console.log('mixed', mixed(false, 5), mixed(true, 6));
console.log('grow', grow(0), grow(1), grow(4), grow(5), grow(22), grow(23), grow(1000));
