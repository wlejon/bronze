// Assigning to an own property of a String exotic object (10.4.3), in both
// modes.
//
// 10.4.3.4 StringCreate gives the object a `length`, and 10.4.3.5
// StringGetOwnProperty gives it one property per index below that length. Every
// one of them is non-writable AND non-configurable, so an assignment to one is
// refused: 10.1.9.2 OrdinarySetWithOwnDescriptor answers false, and 13.15.2
// PutValue step 6.d turns that false into a TypeError for a STRICT reference
// and drops it for a sloppy one. That is the same fork
// `cases/strict_write_refusals` pins for a frozen plain object.
//
// What makes this its own case is where the refusal has to come from. A String
// object is a plain object with a shape here, so the write had nothing to stop
// it and landed as an ordinary slot named "0" -- while every READ consults
// 10.4.3.5 first and answers from the wrapped characters. So `s[0] = "z"` was
// followed by `s[0]` reading "a", and the program held a property that existed
// and could never be observed. A silently unreachable property is the worse
// half of a wrong answer, and it is why this is a refusal rather than a shadow.
//
// The copy path already refused it -- `Object.assign` performs
// `Set(to, key, value, true)` (7.3.25 step 5.c.ii), which throws whatever mode
// the calling code was written in. This is the direct assignment, which is the
// spelling programs actually use.
//
// What each line pins:
//
// 1. A sloppy write to an index and to `length` is discarded, and the reads
//    still answer from the characters.
// 2. A key that is NOT an own property of the string -- an index past the end,
//    or a name -- falls through to the ordinary object and IS stored. That
//    fall-through is what 10.4.3 requires, and it is why the rule cannot be
//    "a String object refuses writes".
// 3. The same two writes from strict code, each a TypeError, caught one at a
//    time so an uncaught first cannot hide the second.
// 4. An ordinary write from strict code still goes through: the flag selects
//    what happens to a REFUSAL and must not change anything else.
// 5. The copy path throws in sloppy code too, because its refusal is the
//    specification's `true` argument and not the caller's mode.
// 6. The final state, read from sloppy code: nothing the refusals touched
//    moved, and everything the fall-through took is there.

function message(f) {
  try {
    f();
    return "no error";
  } catch (e) {
    return e instanceof TypeError ? "TypeError" : "wrong error kind";
  }
}

const s = new String("ab");

s[0] = "z";
s[1] = "z";
s.length = 99;
console.log(s[0], s[1], s.length);

s[2] = "past";
s.tag = "name";
console.log(s[2], s.tag, s.length);

function strictly() {
  "use strict";
  try {
    s[0] = "q";
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  try {
    s.length = 0;
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.name);
  }
  s.tag = "strict";
  s[3] = "also past";
  console.log(s.tag, s[3]);
}
strictly();

console.log(message(function () { return Object.assign(s, { "0": "z" }); }));
console.log(message(function () { return Object.assign(s, { length: 0 }); }));
console.log(message(function () { return Object.assign(s, { fresh: 1 }); }));

console.log(s[0], s[1], s.length, s[2], s[3], s.tag, s.fresh);
console.log(String(s), s.charAt(0), s.valueOf());
