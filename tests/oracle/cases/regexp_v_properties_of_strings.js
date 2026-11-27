// 22.2.1 Table 67 properties of strings under the `v` flag:
// Basic_Emoji, Emoji_Keycap_Sequence, RGI_Emoji_Modifier_Sequence,
// RGI_Emoji_Flag_Sequence, RGI_Emoji_Tag_Sequence, RGI_Emoji_ZWJ_Sequence,
// and RGI_Emoji (union of the other six), sourced from UTS #51 Unicode 16.0.

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
