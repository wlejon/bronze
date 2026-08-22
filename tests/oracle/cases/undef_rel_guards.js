// The undefined-vs-number relational arm (llvm_arith.cpp emitDynamicRel):
// when neither operand pair is both-numbers, an operand mix of numbers and
// `undefined` is answered inline as constant false — 13.10.1's ToPrimitive
// is an identity on both, ToNumeric(undefined) is NaN, and every ordered
// compare against NaN is false. Every guard that must PREVENT the arm is
// pinned below: null (coerces to +0, so `null >= 0` is TRUE — null must
// never take the undefined shortcut), strings, BigInt, and objects whose
// valueOf runs user code that must still fire exactly once per compare.
// Seam: BRONZE_NO_UNDEF_REL=1 routes every one of these back to the helper.

function d(v) { return v; } // launder through a call so inference keeps Dynamic

const u = d(undefined);
const n0 = d(0);
const n1 = d(1.5);
const nan = d(NaN);
const ninf = d(-Infinity);

// ---- the arm itself: undefined against numbers, all four ops --------------
console.log("u>0", u > n0, "u<0", u < n0, "u>=0", u >= n0, "u<=0", u <= n0);
console.log("0>u", n0 > u, "0<u", n0 < u, "0>=u", n0 >= u, "0<=u", n0 <= u);
console.log("u>1.5", u > n1, "1.5<u", n1 < u, "u>=-inf", u >= ninf, "-inf<=u", ninf <= u);
console.log("u>u", u > u, "u<u", u < u, "u>=u", u >= u, "u<=u", u <= u);
console.log("u>nan", u > nan, "nan<=u", nan <= u);

// ---- three.js's exact shape: a missing property against a literal ---------
const material = d({ transparent: false });
console.log("missing>0", material.transmission > 0.0);

// ---- null is NOT undefined: it coerces to +0 and must keep the helper -----
const nl = d(null);
console.log("null>=0", nl >= 0, "null<=0", nl <= 0, "null>0", nl > 0, "null<0", nl < 0);
console.log("null>-1", nl > d(-1), "0>=null", d(0) >= nl, "null>=u", nl >= u, "u<=null", u <= nl);

// ---- strings keep the helper (code-unit order, numeric coercion) ----------
const s = d("10");
console.log("s>9", s > d(9), "s<'9'", s < d("9"), "u>''", u > d(""), "''<=u", d("") <= u);

// ---- booleans keep the helper (ToNumber(true) is 1) -----------------------
console.log("true>0", d(true) > n0, "u>false", u > d(false), "false<=u", d(false) <= u);

// ---- BigInt keeps the helper on both sides --------------------------------
const big = d(10n);
console.log("big>9", big > d(9), "u>big", u > big, "big<=u", big <= u);

// ---- an object's valueOf must still run, exactly once per compare ---------
let calls = 0;
const loud = d({ valueOf() { calls++; return 7; } });
console.log("loud>u", loud > u, "u<loud", u < loud, "loud>=3", loud >= d(3));
console.log("valueOf calls", calls);

// ---- toPrimitive ordering: left operand first for <, and the object still
// runs even when the other side is undefined on either side ----------------
const order = [];
const a = d({ valueOf() { order.push("a"); return 1; } });
const b = d({ valueOf() { order.push("b"); return 2; } });
console.log("a<b", a < b, "b>a", b > a, "order", order.join(","));
order.length = 0;
console.log("a<=u", a <= u, "u>=b", u >= b, "order2", order.join(","));
