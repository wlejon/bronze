// BLOCKED: `for (const k in o)` is `unsupported construct: for-in loop` in
// lowering today. The parser reads the head, and nothing below it does.
//
// This is not the same walk as for-of. for-of asks the container for values
// by index (docs/0012 decision 2); for-in yields own enumerable string KEYS
// in the language's order — integer-like keys ascending, then the rest in
// insertion order (docs/0009 decision 1) — and then continues up the
// prototype chain. So `rtOwnKeysOrdered` is most of the answer already; what
// is missing is a loop form that consumes it, plus the two rules below.
//
// The two rules that make it more than a key walk: a class method is
// NON-enumerable (ECMA-262 15.7.14 defines methods with enumerable: false),
// so `for (const k in p)` sees `x` and not `m`; and `for (const k in null)`
// is a no-op rather than an error (ECMA-262 14.7.5.5 returns an empty
// completion for null and undefined).
const o = { b: 1, a: 2, "2": 3, "1": 4 };
for (const k in o) {
  console.log(k);
}
const arr = [10, 20];
for (const i in arr) {
  console.log(i + ":" + arr[i]);
}
class P {
  constructor() {
    this.x = 1;
  }
  m() {
    return 1;
  }
}
for (const k in new P()) {
  console.log(k);
}
for (const k in null) {
  console.log("never");
}
for (const k in undefined) {
  console.log("never");
}
console.log("done");
