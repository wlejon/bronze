// BLOCKED: bronze has no strict mode. A `"use strict"` directive parses as an
// ordinary string expression statement and changes nothing, so every strict-
// only error below simply does not happen and the case prints the sloppy
// answers instead.
//
// The mechanism this needed was `throw`, and `throw` has landed (docs/0020).
// What is missing now is the DIRECTIVE: ECMA-262 11.2.2 makes strictness a
// property of a script or function body determined by its Directive Prologue,
// and every construct below has two spec'd behaviours selected by it. That is
// a parser and scope-tracking change — a flag threaded from the prologue
// through every function body — not an exceptions change, which is why it is
// its own piece of work rather than part of this chunk.
//
// docs/0019 decision 6 chose the sloppy no-op for a getter-only write and
// said it becomes a TypeError when `throw` lands. That was half right: the
// TypeError is 10.1.9.2 step 3 (OrdinarySetWithOwnDescriptor returns false
// when the property has no setter) combined with 13.15.2 PutValue step 6.d,
// which throws ONLY when the reference is strict. Making it throw
// unconditionally would be wrong for the sloppy code that is bronze's
// default, and `accessor_properties.expected` pins that sloppy answer.
"use strict";

// 10.1.9.2 + 13.15.2: a write to a getter-only property.
const square = {
  side: 3,
  get area() {
    return this.side * this.side;
  }
};
try {
  square.area = 100;
  console.log("no throw");
} catch (e) {
  console.log(e instanceof TypeError, e.name);
}
console.log(square.area);

// 13.15.2 PutValue step 6: an assignment to an unresolvable reference is a
// ReferenceError in strict mode, and creates a global in sloppy mode.
//
// This block alone now prints the pinned line, and not because strictness
// arrived: docs/0027 decision 1 gives bronze one answer for an unresolvable
// reference in every position, and it is the strict one — sloppy mode's
// implicit global was never available, because bronze has no global object to
// create a binding on (docs/0011 decision 1). The case stays blocked on the
// two halves above and below, which are what a Directive Prologue would buy.
try {
  undeclared = 1;
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}

// 8.6.2 / 13.5.1.2: `delete` of an unqualified identifier is an early
// SyntaxError in strict code, so this file would not even parse under a
// strict-aware parser. It is written here as the shape the case will take
// once strictness exists; the two lines above are the runtime half.
