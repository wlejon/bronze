// What `\p{...}` still refuses, and why each refusal says what it says.
//
// bronze carries General_Category, Script and the binary properties of Table
// 69, so a property escape has three ways to fail and they are three different
// messages. A value that names no SCRIPT is diagnosed against the script table
// it was looked up in, because letting it fall through to the category lookup
// would report a misspelled category and send the reader to the wrong table. A
// binary property ECMA-262 does not list is refused with its own name and a
// list of what IS carried. And an unknown or misspelled
// name is a syntax error naming the offender, never a set that quietly matches
// nothing, which is the failure mode a property escape has and `\d` does not.
//
// What the sets MEAN, once they are accepted, is regexp_unicode_property and
// regexp_script_properties; this file is only about the three refusals.
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

// An unknown SCRIPT, diagnosed as one and pointing at the report that defines
// the property. Both spellings of the property reach the same message, which is
// what stops `scx` from being the one that falls somewhere else.
console.log(names(function () { return new RegExp("\\p{Script=Greeek}", "u"); }, "Script"),
            names(function () { return new RegExp("\\p{Script=Greeek}", "u"); }, "UAX #24"));
console.log(names(function () { return new RegExp("\\p{scx=Greeek}", "u"); },
                  "Script_Extensions"));
// And NOT through the General_Category refusal: `Greeek` is no category value
// either, so a lookup that fell through would report a misspelled CATEGORY and
// send the reader to the wrong table.
console.log(names(function () { return new RegExp("\\p{Script=Greeek}", "u"); }, "misspelling"));

// A UAX #44 binary property that Table 69 does not list, named together with
// what IS carried. The ones Table 69 lists are accepted.
console.log(names(function () { return new RegExp("\\p{Hyphen}", "u"); }, "Hyphen"),
            names(function () { return new RegExp("\\p{Hyphen}", "u"); }, "Table 69"));
console.log(names(function () { return new RegExp("\\p{Other_Alphabetic}", "u"); },
                  "Other_Alphabetic"),
            message(function () { return new RegExp("\\p{Alphabetic}", "u"); }) === "no error");

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
