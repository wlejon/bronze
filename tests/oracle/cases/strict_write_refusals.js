// The three ways an ordinary assignment can FAIL without throwing on its own
// (ECMA-262 10.1.9.2), and the fourth thing that shares their mechanism —
// `delete` of a non-configurable property (13.5.1.2 step 5.b). Each is a
// silent no-op in sloppy code and a TypeError in strict code, and 13.15.2
// PutValue step 6.d is the single line that decides which: it throws only when
// the reference is strict.
//
// The file is sloppy and `strictly` writes its own `"use strict"`, so both
// modes run over the SAME two objects in one program. That is the shape the
// pair has to take: strictness is a property of the code, not of the value, so
// a per-file split would prove only that two programs differ.
//
// `cases/strict_mode` holds the accessor-with-no-setter member of the family
// and is where the mechanism is described; this case is the other three.
//
// What each line pins:
//
// 1. A write to a non-writable data property is discarded in sloppy code —
//    10.1.6.3 step 2.a returns false and 13.15.2 drops it.
// 2. A new property on a non-extensible object is discarded too: 10.1.6.3
//    step 2.b, the same false through the same drop.
// 3. …and it is not merely invisible, it never happened: `Object.keys` still
//    reports one key.
// 4. `delete` of a non-configurable property ANSWERS false rather than
//    throwing. It is the one operator in this family whose sloppy answer is a
//    value the program can see.
// 5. The property is still there afterwards.
// 6-8. The same three writes from strict code, each a TypeError. They are
//    caught one at a time because an uncaught first would hide the rest, and
//    `e instanceof TypeError` is checked as well as `e.name` so that a
//    correctly-named error of the wrong constructor cannot pass.
// 9. A write that is NOT refused still goes through in strict mode. The flag
//    selects what happens to a REFUSAL, and must not turn an ordinary
//    assignment into anything else.
// 10. The final state, read from sloppy code: the refused writes left no
//    trace, and the accepted one did.
const readOnly = {};
Object.defineProperty(readOnly, "k", {
  value: 1,
  writable: false,
  enumerable: true,
  configurable: false
});

const closed = { a: 1 };
Object.preventExtensions(closed);

readOnly.k = 2;
console.log(readOnly.k);
closed.b = 2;
console.log(closed.b);
console.log(Object.keys(closed).join(","));
console.log(delete readOnly.k);
console.log(readOnly.k);

function strictly() {
  "use strict";
  try {
    readOnly.k = 3;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    closed.c = 3;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    delete readOnly.k;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  closed.a = 9;
  console.log(closed.a);
}
strictly();

console.log(readOnly.k, closed.a, closed.c);
