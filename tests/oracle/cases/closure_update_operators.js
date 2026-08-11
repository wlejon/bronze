// `++x` and `x--` on a captured binding.
//
// Derived from ECMA-262 13.4: an update expression evaluates its target to a
// Reference, takes ToNumeric(GetValue(ref)) as the old value, and PutValue's
// old +/- 1. The prefix form yields the NEW value, the postfix form the old
// one. Nothing in that is special to an SSA-backed binding, so a binding that
// lives in an environment record (docs/0007) must be read and written through
// the record exactly as `n = n + 1` already is — the update path had its own
// lookup that only ever consulted the current function's variables.
//
// Pinned here: prefix and postfix, increment and decrement, on a binding
// captured from an enclosing function, from two levels down, and at module
// scope; plus a plain write and a compound assign to the same bindings, which
// are the operations the update forms must agree with.

function makeCounter() {
  let n = 0;
  return {
    inc: () => ++n,
    dec: () => --n,
    post: () => n++,
    get: () => n
  };
}

const c = makeCounter();
console.log(c.inc());
console.log(c.inc());
console.log(c.post());
console.log(c.get());
console.log(c.dec());
console.log(c.get());

// Each call to makeCounter creates its own `n`: the closures capture the
// binding, and a second activation is a second binding.
const d = makeCounter();
console.log(d.inc());
console.log(c.get());

// Two levels of nesting: `inner` reaches past `middle` to `outer2`'s binding,
// and `outer2` sees the write when it reads `k` afterwards.
function outer2() {
  let k = 10;
  function middle() {
    function inner() { return k++; }
    return inner();
  }
  const first = middle();
  return [first, k];
}
console.log(outer2());

// Module scope: the same binding written by a top-level statement and by
// three different top-level functions.
let m = 0;

function bumpM() { return ++m; }
function readM() { return m; }
function addM(delta) { m += delta; }

console.log(bumpM());
m++;
console.log(readM());
console.log(m--);
console.log(readM());
console.log(--m);
addM(5);
console.log(readM());
m = 42;
console.log(readM());
console.log(m++);
console.log(bumpM());

// A captured PARAMETER is a binding like any other.
function fromParam(start) {
  const step = () => start++;
  step();
  step();
  return [start, step()];
}
console.log(fromParam(1));
