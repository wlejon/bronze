// Strict mode, selected by the Directive Prologue (ECMA-262 11.2.2): the
// leading run of ExpressionStatements that are string literals, of which
// `"use strict"` is the one that means something. Strictness is a property of
// the CODE — a Script or a function body — fixed at parse time and inherited
// by everything written inside; it is never a property of a call.
//
// The directive counts only if the literal contains no escape sequences, which
// is why the parser compares the RAW source text between the quotes rather
// than the decoded value: `"use strict"` denotes the same characters and
// selects nothing. That negative is pinned by `cases/strict_directive_escaped`,
// beside the three other cases this one does not reach:
// `strict_write_refusals`, `strict_class_body` and `strict_per_function`.
//
// What each line pins:
//
// 1. A write to a getter-only property. 10.1.9.2 step 5.c returns false when
//    the accessor has no `set` half, and 13.15.2 PutValue step 6.d turns that
//    false into a TypeError — but ONLY when the reference is strict. The
//    sloppy reading is a silent no-op, and it is still what
//    `cases/accessor_properties` pins; the two cases hold the two halves of
//    one rule and neither is a special case of the other. That is why the
//    strict flag rides on the `prop.set` INSTRUCTION rather than on the
//    runtime: one program holds both modes at once.
// 2. The property afterwards, which the getter still computes from `side` —
//    the throw happened INSTEAD of the write, not after it.
// 3. An assignment to an unresolvable reference. 13.15.2 PutValue step 6 makes
//    it a ReferenceError in strict mode and creates a global in sloppy mode,
//    and this line printed the strict answer before strictness existed:
//    bronze has one answer for an unresolvable reference in every position,
//    because it has no global object to create an implicit binding on. It is
//    here as a ratchet on that, not as evidence the directive did anything.
//
// `delete` of an unqualified identifier is the rule this file is silent about,
// and deliberately: 13.5.1.1 makes it an early SyntaxError in strict code, so
// a case that ran one could not run at all. An early error is pinned where
// early errors are pinned, in tests/parse.
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
// bronze has one answer for it in every position and it is the strict one:
// sloppy mode's implicit global was never available, because there is no
// global object to create a binding on. So this block is a ratchet on an
// answer that did not change rather than a demonstration of one that did —
// and it is pinned here so that a future global object cannot quietly make
// the sloppy reading available to strict code.
try {
  undeclared = 1;
  console.log("no throw");
} catch (e) {
  console.log(e.name);
}
