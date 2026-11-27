// Static semantics, set algebra, and case-folding over Table 67 properties of strings.
function errName(src, flags) {
  try {
    new RegExp(src, flags);
    return "no-throw";
  } catch (e) {
    return e.constructor.name;
  }
}

console.log(errName("\\P{Basic_Emoji}", "v"));
console.log(errName("[^[\\p{RGI_Emoji_Tag_Sequence}--\\p{RGI_Emoji_Tag_Sequence}]]", "v"));

// Intersecting a property of strings with a character-only class clears MayContainStrings,
// so negating the intersection is legal.
const notAsciiEmoji = /^[^[\p{Basic_Emoji}&&\p{ASCII}]]$/v;
console.log(notAsciiEmoji.test("a"), notAsciiEmoji.test("\u{1F600}"));

// Intersection keeps common members across Table 67 sets.
const inter = /^[\p{RGI_Emoji}&&\p{Basic_Emoji}]$/v;
console.log(
  inter.test("\u{1F600}"),
  inter.test("\u{2764}\u{FE0F}"),
  inter.test("\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F466}")
);

// 22.2.2.9 MaybeSimpleCaseFolding under `vi` applies element-by-element to strings:
// U+24C2 U+FE0F is in Basic_Emoji, and simpleCaseFold(U+24C2) === U+24DC.
const circledRe = /^\p{Basic_Emoji}$/vi;
console.log(circledRe.test("\u{24C2}\u{FE0F}"), circledRe.test("\u{24DC}\u{FE0F}"));
console.log(/^[\q{ABC}]$/vi.test("abc"), /^[\q{ABC}--\q{abc}]$/vi.test("abc"));

// Global match scans mixed ASCII and multi-codepoint emoji sequences cleanly.
const m = "hi \u{1F600} \u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F466} ok".match(/\p{RGI_Emoji}/gv);
console.log(m.length, m[0] === "\u{1F600}", m[1] === "\u{1F468}\u{200D}\u{1F469}\u{200D}\u{1F466}");
