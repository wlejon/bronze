// `for (const k in o)` — the enumeration `for-of` is not (docs/0018 decision 1).
//
// What this pins, and where each answer comes from:
//
// 1. ORDER. Own keys come out in the language's order — integer-like keys
//    ascending, then the rest in insertion order (docs/0009 decision 1) — so
//    `{ b: 1, a: 2, "2": 3, "1": 4 }` yields 1, 2, b, a and not source order.
// 2. AN ARRAY'S INDICES ARE STRINGS. `for (const i in arr)` binds "0" and
//    "1", not 0 and 1, so `i + ":"` concatenates rather than adding — and
//    `arr[i]` still reaches the element, because ToPropertyKey makes `arr[0]`
//    and `arr["0"]` the same property.
// 3. A CLASS METHOD IS NOT ENUMERABLE. ECMA-262 15.7.14 defines methods with
//    `enumerable: false`, so an instance of `P` yields `x` and never `m`,
//    even though `m` is found on its prototype by an ordinary read.
// 4. NULL AND UNDEFINED ARE A NO-OP. ECMA-262 14.7.5.5 returns an empty
//    completion for them rather than throwing — one of the few places the
//    language is deliberately forgiving — so the body never runs and the
//    program continues to "done".
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
