// BLOCKED: `invalid regular expression: unsupported: `\p{Basic_Emoji}` is a
// property of STRINGS (22.2.1's Table 67), which is legal only under the `v`
// flag. bronze holds class members that are strings — `\q{...}` works — but
// carries no table for any of the seven, each of which is a list of emoji
// sequences from UTS #51`.
//
// The other half of 22.2.1's string-capable sets. `\q{...}` writes a set of
// strings by hand and works (`cases/regexp_v_strings.js`); these seven name one
// from Unicode's own data: Basic_Emoji, Emoji_Keycap_Sequence,
// RGI_Emoji_Modifier_Sequence, RGI_Emoji_Flag_Sequence, RGI_Emoji_Tag_Sequence,
// RGI_Emoji_ZWJ_Sequence, and RGI_Emoji, which is the union of the other six.
// They are legal ONLY under `v`, because only there can a class hold a member
// that is more than one character — and for the same reason a negated class
// cannot contain one.
//
// What blocks them is now DATA rather than representation. The class machinery
// already holds string members, sorts them longest-first, and runs `--` and `&&`
// over them. What is missing is the tables: `emoji-sequences.txt` and
// `emoji-zwj-sequences.txt` from UTS #51, thousands of sequences across six
// properties, plus a generator arm for each and the emoji-version pinning that
// makes the answers reproducible.
//
// Every expectation below is a sequence documented in UTS #51's RGI set, spelled
// with `\u{...}` escapes so this file stays ASCII. The pins are deliberately
// conservative: one certain member per property, one non-member where the
// distinction is interesting (U+2764 is in Basic_Emoji only WITH U+FE0F, since
// its default presentation is text), and the two early errors.

const grin = "\u{1F600}";
const heart = "\u{2764}";
const heartEmoji = "\u{2764}\u{FE0F}";
const keycap = "1\u{FE0F}\u{20E3}";
const flagUS = "\u{1F1FA}\u{1F1F8}";
const flagScotland = "\u{1F3F4}\u{E0067}\u{E0062}\u{E0073}\u{E0063}\u{E0074}\u{E007F}";
const thumbUp = "\u{1F44D}\u{1F3FE}";
const family = "\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F466}";

// A character with default emoji presentation is a Basic_Emoji on its own; one
// without it is a Basic_Emoji only as a two-element presentation sequence.
console.log(/^\p{Basic_Emoji}$/v.test(grin), /^\p{Basic_Emoji}$/v.test("a"));
console.log(/^\p{Basic_Emoji}$/v.test(heartEmoji), /^\p{Basic_Emoji}$/v.test(heart));

// One member of each of the other five named properties.
console.log(/^\p{Emoji_Keycap_Sequence}$/v.test(keycap));
console.log(/^\p{RGI_Emoji_Flag_Sequence}$/v.test(flagUS));
console.log(/^\p{RGI_Emoji_Tag_Sequence}$/v.test(flagScotland));
console.log(/^\p{RGI_Emoji_Modifier_Sequence}$/v.test(thumbUp));
console.log(/^\p{RGI_Emoji_ZWJ_Sequence}$/v.test(family));

// RGI_Emoji is the union of the six.
console.log(/^\p{RGI_Emoji}$/v.test(family), /^\p{RGI_Emoji}$/v.test(grin));

// And the point of putting them in a class at all: the set operators work over
// a property of strings and a hand-written `\q{...}` alike.
console.log(
  /^[\p{RGI_Emoji_Tag_Sequence}--\q{\u{1F3F4}\u{E0067}\u{E0062}\u{E0073}\u{E0063}\u{E0074}\u{E007F}}]$/v
    .test(flagScotland));

// Two early errors: a property of strings needs `v`, and a class that may
// contain strings cannot be negated (22.2.1's static semantics, both).
function err(src, flags) {
  try {
    new RegExp(src, flags);
    return "no-throw";
  } catch (e) {
    return e.constructor.name;
  }
}
console.log(err("\\p{Basic_Emoji}", "u"), err("[^\\p{Basic_Emoji}]", "v"));
