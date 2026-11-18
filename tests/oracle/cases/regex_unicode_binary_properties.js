// The binary Unicode properties of ECMA-262 Table 69, by canonical name and by
// alias, under `u` and `v`.
//
// Each row names code points the property holds and code points it does not.
// They are chosen from characters whose properties did not change between
// Unicode 15.1 and 16.0, so the answer does not depend on which UCD an engine
// carries. For each spelling, `\p{...}` must match every `in` and no `out`,
// `\P{...}` the reverse, and the alias must agree with the canonical name.

const table = [
  ["ASCII", "", [0x41, 0x7f, 0x00], [0x80, 0xe9, 0x1f600]],
  ["ASCII_Hex_Digit", "AHex", [0x30, 0x46, 0x66], [0x47, 0xff10, 0x660]],
  ["Alphabetic", "Alpha", [0x41, 0x345, 0x4e00, 0x2160], [0x30, 0x20, 0x1f600]],
  ["Any", "", [0x00, 0x10ffff, 0xd800], []],
  ["Assigned", "", [0x41, 0xe000, 0x1f600], [0x378, 0xe0080, 0xffff]],
  ["Bidi_Control", "Bidi_C", [0x200e, 0x202a, 0x2066], [0x200b, 0x41]],
  ["Bidi_Mirrored", "Bidi_M", [0x28, 0x29, 0x3c, 0x2208], [0x41, 0x2d]],
  ["Case_Ignorable", "CI", [0x27, 0x2e, 0x300, 0xad], [0x41, 0x30]],
  ["Cased", "", [0x41, 0x61, 0x1c5, 0x2160], [0x30, 0x4e00, 0x5d0]],
  ["Changes_When_Casefolded", "CWCF", [0x41, 0xdf, 0x3c2], [0x61, 0x30]],
  ["Changes_When_Casemapped", "CWCM", [0x41, 0x61, 0xdf], [0x30, 0x4e00, 0xaa]],
  ["Changes_When_Lowercased", "CWL", [0x41, 0x1c5, 0x391], [0x61, 0xdf]],
  ["Changes_When_NFKC_Casefolded", "CWKCF", [0x41, 0xad, 0xa0, 0xfb01], [0x61, 0x20, 0x4e00]],
  ["Changes_When_Titlecased", "CWT", [0x61, 0xdf, 0x1c6], [0x41, 0x1c5, 0x30]],
  ["Changes_When_Uppercased", "CWU", [0x61, 0xdf, 0x1c5], [0x41, 0x30]],
  ["Dash", "", [0x2d, 0x2014, 0x2212], [0x5f, 0x41]],
  ["Default_Ignorable_Code_Point", "DI", [0xad, 0x200b, 0xfe0f], [0x20, 0x41]],
  ["Deprecated", "Dep", [0x149, 0xe0001, 0x206a], [0x41, 0x2d]],
  ["Diacritic", "Dia", [0x5e, 0x60, 0x300], [0x41, 0x2d]],
  ["Emoji", "", [0x23, 0x30, 0xa9, 0x1f600], [0x41, 0x2d]],
  ["Emoji_Component", "EComp", [0x23, 0x200d, 0x1f3fb, 0x1f1e6], [0x41, 0x1f600]],
  ["Emoji_Modifier", "EMod", [0x1f3fb, 0x1f3ff], [0x1f600, 0x41]],
  ["Emoji_Modifier_Base", "EBase", [0x261d, 0x1f44d], [0x1f600, 0x41]],
  ["Emoji_Presentation", "EPres", [0x231a, 0x1f600], [0x23, 0xa9, 0x41]],
  ["Extended_Pictographic", "ExtPict", [0xa9, 0x1f600, 0x1fffd], [0x23, 0x41]],
  ["Extender", "Ext", [0xb7, 0x3005, 0x640], [0x41, 0x2d]],
  ["Grapheme_Base", "Gr_Base", [0x41, 0x20, 0x4e00], [0x300, 0x0a, 0x200d]],
  ["Grapheme_Extend", "Gr_Ext", [0x300, 0x200c, 0xff9e], [0x41, 0x200d]],
  ["Hex_Digit", "Hex", [0x30, 0x61, 0xff10, 0xff21], [0x67, 0x660]],
  ["IDS_Binary_Operator", "IDSB", [0x2ff0, 0x2ffb], [0x2ff2, 0x41]],
  ["IDS_Trinary_Operator", "IDST", [0x2ff2, 0x2ff3], [0x2ff0, 0x41]],
  ["ID_Continue", "IDC", [0x41, 0x30, 0x5f, 0x300], [0x24, 0x2d, 0x20]],
  ["ID_Start", "IDS", [0x41, 0x2160, 0x4e00, 0x309b], [0x30, 0x5f, 0x24]],
  ["Ideographic", "Ideo", [0x4e00, 0x3006, 0x3007, 0x20000], [0x41, 0x3041, 0x2e80]],
  ["Join_Control", "Join_C", [0x200c, 0x200d], [0x200b, 0x200e]],
  ["Logical_Order_Exception", "LOE", [0xe40, 0xe44, 0xaab5], [0xe01, 0x41]],
  ["Lowercase", "Lower", [0x61, 0xaa, 0x2170], [0x41, 0x1c5, 0x30]],
  ["Math", "", [0x2b, 0x3d, 0x5e, 0x2211], [0x2d, 0x41]],
  ["Noncharacter_Code_Point", "NChar", [0xfdd0, 0xfffe, 0x10ffff], [0xfffd, 0x41]],
  ["Pattern_Syntax", "Pat_Syn", [0x21, 0x2f, 0x2190], [0x41, 0x30, 0x5f]],
  ["Pattern_White_Space", "Pat_WS", [0x20, 0x09, 0x200e, 0x2029], [0xa0, 0x3000]],
  ["Quotation_Mark", "QMark", [0x22, 0x27, 0xab, 0x300c], [0x60, 0x41]],
  ["Radical", "", [0x2e80, 0x2f00, 0x2fd5], [0x4e00, 0x41]],
  ["Regional_Indicator", "RI", [0x1f1e6, 0x1f1ff], [0x1f1e5, 0x41]],
  ["Sentence_Terminal", "STerm", [0x21, 0x2e, 0x3f, 0x3002], [0x2c, 0x3b]],
  ["Soft_Dotted", "SD", [0x69, 0x6a, 0x456], [0x131, 0x49]],
  ["Terminal_Punctuation", "Term", [0x21, 0x2c, 0x3b, 0x3002], [0x2d, 0x41]],
  ["Unified_Ideograph", "UIdeo", [0x4e00, 0xfa0e, 0x20000], [0x3006, 0x3007, 0x41]],
  ["Uppercase", "Upper", [0x41, 0x2160, 0x24b6], [0x61, 0x1c5]],
  ["Variation_Selector", "VS", [0xfe00, 0xfe0f, 0xe0100, 0x180b], [0x200d, 0x41]],
  ["White_Space", "space", [0x20, 0x09, 0xa0, 0x3000, 0x85], [0x200b, 0x41, 0x180e]],
  ["XID_Continue", "XIDC", [0x41, 0x30, 0x5f, 0xb7], [0x37a, 0x24]],
  ["XID_Start", "XIDS", [0x41, 0x4e00], [0x37a, 0x30, 0x309b]],
];

function hex(cp) {
  return "U+" + cp.toString(16).toUpperCase().padStart(4, "0");
}

function check(spelling, flags, ins, outs) {
  const p = new RegExp("^\\p{" + spelling + "}$", flags);
  const P = new RegExp("^\\P{" + spelling + "}$", flags);
  const wrong = [];
  for (const cp of ins) {
    const s = String.fromCodePoint(cp);
    if (!p.test(s) || P.test(s)) wrong.push("in " + hex(cp));
  }
  for (const cp of outs) {
    const s = String.fromCodePoint(cp);
    if (p.test(s) || !P.test(s)) wrong.push("out " + hex(cp));
  }
  return wrong.length === 0 ? "ok" : wrong.join(",");
}

for (const [name, alias, ins, outs] of table) {
  const parts = [name, check(name, "u", ins, outs), check(name, "v", ins, outs)];
  if (alias) parts.push(alias, check(alias, "u", ins, outs), check(alias, "v", ins, outs));
  console.log(parts.join(" "));
}

// The escape inside a class, and inside `v`-mode class set expressions.
console.log(/^[\p{Ideo}a]+$/u.test("a一〇"), /^[^\p{Ideo}]$/u.test("一"));
console.log(/^[\p{Ideo}--\p{UIdeo}]$/v.test("〇"), /^[\p{Ideo}--\p{UIdeo}]$/v.test("一"));
console.log(/^[\p{Emoji}&&\p{ASCII}]+$/v.test("#*0123456789"), /^[\p{Emoji}&&\p{ASCII}]$/v.test("a"));
console.log(/^[\p{White_Space}\p{Dash}]+$/v.test(" -—　"), /^[^\P{Alpha}]$/v.test("x"));
console.log("MapLibre text".match(/\p{Ideographic}|\p{Ideo}/gu), "地図".match(/\p{Ideo}/gu).length);

// Case-insensitive: under `u` and `i` a property set is closed under simple
// case folding, so a lowercase-only property matches the uppercase letter too.
console.log(/^\p{Lower}$/ui.test("A"), /^\p{Upper}$/ui.test("a"), /^\p{Lower}$/u.test("A"));

// A property's code points beyond the BMP, matched as one code point.
console.log(/^\p{Emoji_Presentation}$/u.test("\u{1f680}"), /^\p{RI}{2}$/u.test("\u{1f1ef}\u{1f1f5}"));
console.log("a\u{1f600}b".replace(/\p{ExtPict}/gu, "*"));

// The names are matched exactly (22.2.1 UnicodeMatchProperty): no loose
// matching, no binary property in the name=value form, and no UAX #44 binary
// property Table 69 does not list.
function kind(source, flags) {
  try {
    new RegExp(source, flags);
    return "ok";
  } catch (e) {
    return e instanceof SyntaxError ? "SyntaxError" : "other";
  }
}
for (const source of ["\\p{alpha}", "\\p{ideographic}", "\\p{Ideographic }", "\\p{White Space}",
                      "\\p{WhiteSpace}", "\\p{ID_START}", "\\p{Script=Ideo}", "\\p{gc=Alphabetic}",
                      "\\p{Alphabetic=Yes}", "\\p{Hyphen}", "\\p{Other_Alphabetic}",
                      "\\p{IDS_Unary_Operator}", "\\p{Prepended_Concatenation_Mark}",
                      "\\p{Grapheme_Link}", "\\p{Full_Composition_Exclusion}"]) {
  console.log(source, kind(source, "u"), kind(source, "v"));
}
