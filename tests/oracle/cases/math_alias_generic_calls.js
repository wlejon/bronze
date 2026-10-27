// A `const` alias of a Math member whose calls are not all lowered as the
// builtin's op: no argument, a string, an object with valueOf. Those calls read
// the binding, so it must still hold the builtin.
function m() {
  const fl = Math.floor;
  return [fl(7.9), fl("7.9"), fl({ valueOf() { return 2.5; } }), fl()];
}
console.log(m());
function n(x) {
  const sq = Math.sqrt;
  return sq(x) + sq(16);
}
console.log(n(9), n("25"));
