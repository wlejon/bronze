// A function may write its own `"use strict"` inside sloppy surroundings, and
// then IT is strict and everything written inside it is strict — including
// functions declared in its body, arrows, and object-literal methods, none of
// which say anything themselves (ECMA-262 11.2.2, and 10.2.5 for the
// inheritance).
//
// The file has no directive at all, so this is the shape a real codebase takes
// before it converts: one function opts in and its neighbours do not.
//
// The write under test is to a getter-only property, which 10.1.9.2 step 5.c
// refuses; 13.15.2 PutValue step 6.d is what turns the refusal into a
// TypeError, and only for a strict reference.
//
// What each line pins:
//
// 1. An ordinary function in a sloppy file is sloppy: the write is discarded
//    and the getter still says 1.
// 2. Three functions written INSIDE a `"use strict"` body, none of which
//    carries a directive: a nested declaration, an arrow, and an
//    object-literal method. All three throw, so strictness is inherited by
//    every function form and not just by the one the directive is attached
//    to. They are collected into one line so that a mode leaking into only
//    some of them is a visible difference rather than a missing line.
// 3. A string-literal statement is a DIRECTIVE only in the prologue. Put a
//    real statement in front of it and it is an ordinary expression
//    statement that evaluates a string and throws it away — so this function
//    is sloppy and its write is discarded, and the getter still says 1.
// 4. The strictness of a function is decided where it is WRITTEN, not where it
//    is called from: `escapee` is sloppy, and calling it from strict code
//    does not make it strict. That is the whole content of "strictness is a
//    property of the code, not of the call".
const target = {
  get g() {
    return 1;
  }
};

function sloppyWrite() {
  target.g = 2;
  return "no throw";
}
console.log(sloppyWrite(), target.g);

function escapee() {
  target.g = 3;
  return "no throw";
}

function strictOuter() {
  "use strict";
  function inner() {
    target.g = 4;
  }
  const arrow = () => {
    target.g = 5;
  };
  const holder = {
    method() {
      target.g = 6;
    }
  };
  const results = [];
  const fns = [inner, arrow, holder.method];
  for (const f of fns) {
    try {
      f();
      results.push("no throw");
    } catch (e) {
      results.push(e.name);
    }
  }
  return results.join(",");
}
console.log(strictOuter());

function notADirective() {
  const x = 1;
  "use strict";
  target.g = x;
  return "no throw";
}
console.log(notADirective(), target.g);

function callsSloppyFromStrict() {
  "use strict";
  return escapee();
}
console.log(callsSloppyFromStrict(), target.g);
