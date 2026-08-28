// The CHECKED UNBOX as a candidate. `*`, `-`, `/` and `%` over unproven
// operands take the numeric arm (`lower_expr_binary.cpp`), so what is boxed in
// `p * q` is not the multiply — it is already an `f64` — but the ToNumber in
// front of each operand. The guarded-region pass spends one `is.number` on `p`
// and DELETES every checked `unbox.f64` of it in the fast copy, however many
// there are.
//
// No BigInt is spelled anywhere in this file, deliberately: one would turn the
// numeric arm off for the whole module and there would be no checked unbox to
// rewrite. `guarded_region_param` is the half that spells one.

const order = [];

function mk(name, value) {
  return {
    valueOf() {
      order.push(name);
      return value;
    },
  };
}

// `p` is read by TWO coercions and `q` and `r` by one each. Four checked
// unboxes, three guards.
function chain(p, q, r) {
  return p * q - r * p;
}

// All Numbers: the guards hold, the four coercions are gone, and 15 - 6 is 9.
console.log(chain(3, 5, 2));

// One operand is an object whose `valueOf` returns a NUMBER. The guard on it
// FAILS — an object is not a Number — so the whole slow copy runs and every
// checked unbox performs its ToNumber, exactly once per operator use and in
// source order. 13.15.3 coerces the left operand before the right, and `-`
// evaluates `p * q` entirely before `r * p`: a, b, c, a.
order.length = 0;
console.log(chain(mk('a', 3), mk('b', 5), mk('c', 2)));
console.log(order.join(','));

// Only the middle operand is an object. The guard chain still fails, so the
// slow copy still runs every coercion — the two that are Numbers call nothing,
// which is why the log has one entry and not four.
order.length = 0;
console.log(chain(3, mk('b', 5), 2));
console.log(order.join(','));

// A `valueOf` that THROWS. It throws from the slow copy, at the coercion the
// original program would have thrown at, with the coercions before it having
// run and none after.
const boom = {
  valueOf() {
    order.push('boom');
    throw new RangeError('nope');
  },
};

order.length = 0;
try {
  chain(mk('a', 3), boom, mk('c', 2));
  console.log('no throw');
} catch (e) {
  console.log(e instanceof RangeError);
}
console.log(order.join(','));

// THE SAME VALUE, THREE TIMES. `p * p * p` is three checked unboxes of one
// parameter and one guard; the fast copy holds one bitcast. An object proves
// the slow copy still runs all three.
function cube(p) {
  return p * p * p;
}

console.log(cube(4));
order.length = 0;
console.log(cube(mk('p', 3)));
console.log(order.join(','));

// `-` as a UNARY operator stays boxed (it is not part of the numeric arm), so
// this is the closure over boxed arithmetic and its promotion is `fneg`. IEEE
// says (-0) + (-0) is -0, and `box.f64` does not lose it.
function negSum(p, q) {
  return -p + -q;
}

console.log(negSum(3, 4));
console.log(Object.is(negSum(0, 0), -0));
console.log(1 / negSum(0, 0));

// A candidate defined AFTER a call that can collect: the guards coalesce at the
// first use, which is after both calls, and the trampoline carries no promoted
// value because every product comes later.
function afterCalls(o) {
  const a = o.f();
  const b = o.g();
  return a * b + a * 2;
}

console.log(afterCalls({ f: () => 7, g: () => 3 }));
order.length = 0;
console.log(afterCalls({ f: () => mk('a', 7), g: () => 3 }));
console.log(order.join(','));
