// A refused write to a FROZEN ARRAY and to a FROZEN FUNCTION, from sloppy code
// and from strict code — `cases/strict_write_refusals` for the two receiver
// kinds whose own properties are not in a shape.
//
// 13.15.2 PutValue step 6.d is the single line that decides: an ordinary
// assignment that 10.1.9.2 answered false to is discarded for a sloppy
// reference and is a TypeError for a strict one. Nothing about that depends on
// where the receiver keeps its properties, which is exactly why it is worth
// pinning here: an array's elements live in its element block and a function's
// statics in a side object, and a `freeze` that reached neither used to make
// every line below a silent success in BOTH modes.
//
// The file is sloppy and `strictly` writes its own `"use strict"`, so both
// modes run over the same three objects in one program — strictness is a
// property of the code and not of the value, and a per-file split would prove
// only that two programs differ.
//
// What each line pins:
//
// 1. A write to a frozen array's existing element is discarded — 10.1.6.3 step
//    2.a, the array's elements being non-writable.
// 2. The same write through the two spellings a compiled program can take:
//    `a[0]` with a literal index and `a[i]` with a computed one reach different
//    runtime helpers, and a refusal that only one of them honoured would be a
//    hole a program falls through by renaming a variable.
// 3. An index at the end of a non-extensible array is a CREATE, so it is
//    refused by extensibility rather than by writability — 10.1.6.3 step 2.b —
//    and `length` does not move.
// 4. A frozen function's static is a non-writable data property, and a new one
//    is an add to a non-extensible object: the same two refusals as an object's.
// 5. `prototype` is the function's own property that lives in a slot rather
//    than in the statics table (10.2.4 makes it non-configurable and writable),
//    so freezing has to reach it separately or the assignment lands.
// 6. `delete` of a frozen array's element ANSWERS false in sloppy code, the one
//    refusal in this family a sloppy program sees as a value — and a SEALED
//    array's element answers false too, since seal is what makes an element
//    non-configurable.
// 7-13. The same seven refusals from strict code, each a TypeError, caught one
//    at a time because an uncaught first would hide the rest.
// 14. A write that is NOT refused still goes through in strict mode: a sealed
//    array's elements are writable, and the strict flag selects what happens to
//    a REFUSAL rather than turning every assignment into one.
// 15. The final state, read from sloppy code: the refused writes left no trace.

const frozenArray = Object.freeze([1, 2]);
const sealedArray = Object.seal([1, 2]);
const closedArray = Object.preventExtensions([1, 2]);

function frozenFn() {}
frozenFn.tag = "t";
const fnProto = frozenFn.prototype;
Object.freeze(frozenFn);

const one = 1;
frozenArray[0] = 9;
frozenArray[one] = 9;
frozenArray["0"] = 9;
closedArray[2] = 3;
frozenFn.tag = "changed";
frozenFn.other = 1;
frozenFn.prototype = { replaced: true };

console.log(frozenArray[0], frozenArray[1], closedArray[2], closedArray.length);
console.log(frozenFn.tag, frozenFn.other, frozenFn.prototype === fnProto);
console.log(delete frozenArray[0], frozenArray[0]);
console.log(delete sealedArray[0], sealedArray[0]);

function strictly() {
  "use strict";
  try {
    frozenArray[0] = 9;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    frozenArray[one] = 9;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    closedArray[2] = 3;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    frozenFn.tag = "changed";
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    frozenFn.other = 1;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    frozenFn.prototype = { replaced: true };
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    delete frozenArray[0];
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  sealedArray[0] = 7;
  console.log(sealedArray[0]);
}
strictly();

console.log(frozenArray.join(","), sealedArray.join(","), closedArray.length);
console.log(frozenFn.tag, frozenFn.prototype === fnProto);
