// The edges of the RegExp object itself (ECMA-262 22.2.3 and 22.2.6) — the
// constructor's three ways of being called, the empty pattern, the failures
// that are catchable, and the empty match, which is the one thing every
// pattern-driven loop has to get right or it does not terminate.
//
// Expectations derived from: 22.2.3.1 RegExp(pattern, flags) (a RegExp
// argument donates its source, and its flags only when the second argument is
// undefined), 22.2.6.10 EscapeRegExpPattern (an empty pattern's `source` is
// "(?:)" so that the source form is still a valid literal), 22.2.3.1 step 12
// (a pattern or a flag string that does not parse is a SyntaxError, thrown and
// therefore catchable), 22.2.6.11 [@@replace] and 22.2.6.8 [@@match] with
// AdvanceStringIndex (an empty match still moves the cursor by one), and
// 22.2.2.3 (an alternative that was not taken leaves its groups unset).
const built = new RegExp("a" + "b+", "gi");
console.log(built.source, built.flags, built.test("XABBY"));
console.log(new RegExp(/a/g).flags, new RegExp(/a/g, "i").flags);
console.log(new RegExp(/a.c/g).source);
console.log(new RegExp().source, new RegExp("").source);
console.log(new RegExp("a/b").test("xa/bx"));

try {
  new RegExp("(");
} catch (e) {
  console.log("caught", e.name);
}
try {
  new RegExp("a", "q");
} catch (e) {
  console.log("caught", e.name);
}
try {
  "abc".replaceAll(/b/, "x");
} catch (e) {
  console.log("caught", e.name);
}

// An empty match is a match: it participates, and then the cursor advances by
// one so the loop makes progress.
console.log("abc".replace(/(?:)/g, "-"));
console.log("abc".match(/(?:)/g).length);
console.log("abc".split(/(?:)/).join("|"));

const alt = /(a)|(b)/.exec("b");
console.log(alt[0], alt[1], alt[2], alt.length);

console.log(/a\/b/gi);
console.log(typeof /a/, typeof /a/.exec);

// A literal is a fresh object every time it is evaluated (22.2.4.1), so the
// cursor one call left behind is not the cursor the next call starts with.
function cursorOf() {
  const re = /a/g;
  re.test("aa");
  return re.lastIndex;
}
console.log(cursorOf(), cursorOf());
