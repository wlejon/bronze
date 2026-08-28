// The ORDER user code runs in around a guarded numeric region's guard.
//
// `is.number` reads bits: it calls nothing, allocates nothing and cannot raise.
// So no `valueOf` and no `toString` may run because of a guard — the count in
// the log is the count of ADDS, and it would be higher if the test were a
// coercion in disguise.
//
// 13.15.3 ApplyStringOrNumericBinaryOperator asks ToPrimitive with NO hint, so
// 7.1.1 OrdinaryToPrimitive runs `valueOf` before `toString`, and left before
// right. Both facts are visible here as the interleaving of the log.

const log = [];

const numberish = {
  valueOf() { log.push("v"); return 10; },
  toString() { log.push("s"); return "S"; },
};

function sumOne(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a;
  }
  return total;
}

// Three iterations, three failing guards, three ToPrimitive calls — never six.
console.log(sumOne({ a: numberish }, 3));
console.log(log.join(","));

// The left operand is the object and the right is the property: left first.
log.length = 0;
const leftFirst = [];
function mkLog(name, value) {
  return {
    valueOf() { leftFirst.push(name); return value; },
  };
}
function sumTwo(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    total = total + o.a + o.b;
  }
  return total;
}
console.log(sumTwo({ a: mkLog("a", 1), b: mkLog("b", 2) }, 2));
console.log(leftFirst.join(","));

// A property whose object is a Number for the first two iterations and an
// object after: exactly two ToPrimitive calls, one per remaining iteration,
// and none for the iterations the fast path ran.
log.length = 0;
function flip(o, n) {
  let total = 0;
  for (let i = 0; i < n; i++) {
    if (i === 2) {
      o.a = numberish;
    }
    total = total + o.a;
  }
  return total;
}
console.log(flip({ a: 1 }, 4));
console.log(log.join(","));

// An object with only a `toString`: 7.1.1 falls through to it, and the result
// is a String, so the operator concatenates.
log.length = 0;
const stringish = { toString() { log.push("s"); return "S"; } };
console.log(sumOne({ a: stringish }, 2));
console.log(log.join(","));
