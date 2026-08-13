// What `\p{...}` still refuses, and why each refusal says what it says.
//
// bronze carries General_Category and nothing else, so a property escape has
// three ways to fail and they are three different messages. Script is refused
// BY NAME, because it is the property a reader reaches for next and because
// letting it fall through to the value lookup would report a misspelling where
// the truth is a missing table -- the generator behind bronze's tables reads
// Python's `unicodedata`, which carries no Script property at all. A binary
// property is refused with its own name and a list of what IS carried. And an
// unknown or misspelled name is a syntax error naming the offender, never a
// set that quietly matches nothing, which is the failure mode a property
// escape has and `\d` does not.
//
// The escape is also a +UnicodeMode production, so without `u` it is refused
// rather than read as Annex B's `p` followed by a quantified `{L}`. That is
// the same silent-wrong-answer argument as everywhere else: a pattern written
// to match letters would match the letter p.

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

// Script, by name, and pointing at the data file it would need.
console.log(names(function () { return new RegExp("\\p{Script=Greek}", "u"); }, "Script"),
            names(function () { return new RegExp("\\p{Script=Greek}", "u"); }, "UAX #24"));
console.log(names(function () { return new RegExp("\\p{scx=Greek}", "u"); },
                  "Script_Extensions"));
// And NOT through the general refusal: `Greek` is no General_Category value,
// so a lookup that fell through would call this a misspelling.
console.log(names(function () { return new RegExp("\\p{Script=Greek}", "u"); }, "misspelling"));

// A binary property bronze cannot derive, named together with what it can.
console.log(names(function () { return new RegExp("\\p{Alphabetic}", "u"); }, "Alphabetic"),
            names(function () { return new RegExp("\\p{Alphabetic}", "u"); },
                  "General_Category"));
console.log(names(function () { return new RegExp("\\p{White_Space}", "u"); }, "White_Space"),
            names(function () { return new RegExp("\\p{Emoji}", "u"); }, "Emoji"));

// A misspelling. Matching is case SENSITIVE, as 22.2.1 requires, so `lu` is
// not a sloppy `Lu` -- it is a name that does not exist.
console.log(names(function () { return new RegExp("\\p{Lx}", "u"); }, "\\p{Lx}"),
            names(function () { return new RegExp("\\p{lu}", "u"); }, "\\p{lu}"));
console.log(names(function () { return new RegExp("\\p{General_Category=Nope}", "u"); },
                  "Nope"),
            names(function () { return new RegExp("\\p{Bidi_Class=L}", "u"); }, "Bidi_Class"));

// Without `u` the production does not exist, and the refusal says which of the
// two things is missing -- the flag, not the table.
console.log(names(function () { return new RegExp("\\p{L}"); }, "unicode property escapes"),
            names(function () { return new RegExp("\\P{L}"); }, "`u` flag"));
// With it, the same pattern is an ordinary class.
console.log(new RegExp("\\p{L}", "u").test("a"));

// The syntax around the name is read rather than skipped, so a half-written
// escape is a named error and not a literal `p`.
console.log(names(function () { return new RegExp("\\pL", "u"); }, "`{` was expected"),
            names(function () { return new RegExp("\\p{L", "u"); }, "`}` was expected"));
console.log(names(function () { return new RegExp("\\p{}", "u"); }, "names no property"));
