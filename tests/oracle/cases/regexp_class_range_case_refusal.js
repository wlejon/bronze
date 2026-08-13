// A character class RANGE under `i` is refused for every character it
// CONTAINS, not for the two it spells.
//
// bronze carries no Unicode data file, so `canonicalize` implements the blocks
// whose case rules can be written out and checked by hand, and every other
// block is refused at COMPILE time (`cases/regexp_case_fold_blocks`): a fold
// bronze cannot perform is a named error rather than a silent "no match".
//
// Testing only the two ENDPOINTS of a range let that refusal be walked around.
// `[U+00FF-U+2000]` spells two characters that both fold and names every
// refused character between them, so it compiled -- and then answered plain
// containment for U+1E9E, whose fold it had no table for. The fold was SKIPPED
// rather than diagnosed, which is the one outcome the refusal exists to
// prevent, and it is the worse of the two because nothing says it happened.
//
// The diagnostic names a concrete code point, the way the endpoint check
// already did: the LOWEST refused character the range holds. For any range
// reaching past U+017F that is U+0180, the first character of Latin
// Extended-B, and a message saying only "somewhere in this range" could not be
// acted on.
//
// The last lines are the other half of the same rule. A refusal grown to cover
// the blocks bronze DOES fold would be a hard error for nothing, so a range
// threading the holes between the refused blocks still compiles and still
// folds -- Greek, Cyrillic and Armenian each sit in one of those holes, and
// none of the three ranges below touches a refused block.
//
// Every character above U+007F is spelled by escape: a range is refused for
// what lies BETWEEN its endpoints, and that is not something a reader should
// have to identify by eye.

function message(f) {
  try {
    f();
    return "no error";
  } catch (e) {
    return e instanceof SyntaxError ? e.message : "wrong error kind";
  }
}

function names(f, text) {
  return message(f).indexOf(text) >= 0;
}

// Neither endpoint is refused; every character from U+0180 to U+02FF between
// them is.
console.log(names(function () { return new RegExp("[\\u00FF-\\u2000]", "i"); }, "U+0180"));
// The whole code-unit range is a named error now, which is correct and
// intended: it holds every refused character there is.
console.log(names(function () { return new RegExp("[\\u0000-\\uFFFF]", "i"); }, "U+0180"));
// An endpoint INSIDE a block reports that endpoint, not the block's first
// character.
console.log(names(function () { return new RegExp("[\\u0200-\\u0210]", "i"); }, "U+0200"));
// A single refused character is still refused, which is where this started.
console.log(names(function () { return new RegExp("\\u1E9E", "i"); }, "U+1E9E"));

// Without `i` there is no fold to be wrong about, so the same class is an
// ordinary one and matches by containment.
console.log(message(function () { return new RegExp("[\\u0000-\\uFFFF]"); }));
console.log(new RegExp("[\\u00FF-\\u2000]").test("\u1E9E"));

// The blocks bronze folds are untouched. Each range below sits exactly between
// two refused blocks and still folds through the reverse direction of the
// table, which is what a refusal that had grown too far would have taken away.
console.log(message(function () { return new RegExp("[\\u0391-\\u03A9]", "i"); }));
console.log(/[\u0391-\u03A9]/i.test("\u03C9"), /[\u0430-\u044F]/i.test("\u041F"));
console.log(/[\u0561-\u0586]/i.test("\u0556"), /[a-z]/i.test("Q"));
