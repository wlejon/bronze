// The Directive Prologue counts a string literal only if it contains NO escape
// sequences (ECMA-262 11.2.2, and 12.9.4.2's `hasEscapeSequence` on the
// literal's SourceCharacters). `"use\u0020strict"` denotes exactly the
// characters `use strict` and selects nothing at all.
//
// That is why the parser compares the RAW source text between the quotes
// rather than the decoded value: a decoder that has done its job produces the
// same string for both spellings, so a check on the decoded value cannot tell
// them apart and would silently make this file strict. This case is the
// NEGATIVE that pins the distinction — every other strict case would still
// pass with the wrong comparison.
//
// The write under test is to a getter-only property: refused by 10.1.9.2 step
// 5.c, and turned into a TypeError by 13.15.2 PutValue step 6.d only for a
// strict reference.
//
// What each line pins:
//
// 1. The escaped literal at the top of the FILE is not the directive, so the
//    file is sloppy and the write is the silent no-op.
// 2. The same escaped literal in a function prologue is not the directive
//    either. The rule is about the literal, not about where it sits.
// 3. Single quotes are a SPELLING and not an escape, so `'use strict'` is the
//    directive and this function throws.
// 4. A prologue is the whole leading run of string-literal statements, so the
//    directive may be the second of them.
// 5. The final read, from the sloppy file: nothing above wrote anything, and
//    the getter still says 1.
"use\u0020strict";

const target = {
  get g() {
    return 1;
  }
};

target.g = 2;
console.log("file:", target.g);

function escapedInPrologue() {
  "use\u0020strict";
  target.g = 3;
  return "no throw";
}
console.log(escapedInPrologue(), target.g);

function singleQuoted() {
  'use strict';
  try {
    target.g = 4;
    return "no throw";
  } catch (e) {
    return e.name;
  }
}
console.log(singleQuoted(), target.g);

function secondInPrologue() {
  "use asm";
  "use strict";
  try {
    target.g = 5;
    return "no throw";
  } catch (e) {
    return e.name;
  }
}
console.log(secondInPrologue(), target.g);

console.log(target.g);
