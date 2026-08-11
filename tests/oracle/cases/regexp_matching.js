// The pattern grammar and the matcher (ECMA-262 22.2.1 and 22.2.2), driven
// through `exec` so that nothing here depends on console.log's container
// format.
//
// Expectations derived from: 22.2.2.5.1 RepeatMatcher (greedy and lazy, and
// the empty-iteration guard), 22.2.2.7 CharacterSetMatcher and 22.2.2.9
// Canonicalize (the `i` flag), 22.2.2.6 Assertion (`^`, `$`, `\b`, and what
// `m` changes about the first two), 22.2.2.3 (the ordered alternation, which
// is first-match and NOT longest-match), 22.2.2.4 (lookahead), and 22.2.2.9
// BackreferenceMatcher (a group that never participated matches the empty
// string).
function first(re, s) {
  const r = re.exec(s);
  return r === null ? "-" : r[0];
}

console.log(first(/a+/, "baaab"));
console.log(first(/a+?/, "baaab"));
console.log(first(/a*/, "bbb"));
console.log(first(/a{2,3}/, "aaaaa"));
console.log(first(/a{2,}/, "aaaaa"));
console.log(first(/a{2}/, "aaaaa"));
console.log(first(/colou?r/, "a color!"));

console.log(first(/[0-9]+/, "ab123cd"));
console.log(first(/[^0-9]+/, "12abc34"));
console.log(first(/\d+\.\d+/, "pi=3.14!"));
console.log(first(/\w+/, "  a_1-b"));
console.log(first(/[\w-]+/, "  a_1-b  "));
console.log(/\s/.test(" "), /\S/.test(" "));
console.log(/\t/.test("\t"), /\n/.test("x"));

console.log(first(/^abc$/, "abc"));
console.log(first(/^b/, "a\nb"));
console.log(first(/^b/m, "a\nb"));
console.log(first(/\bword\b/, "a word here"));
console.log(first(/\Bord/, "a word here"));

// `.` is every unit but a line terminator until `s` says otherwise.
console.log(/a.b/.test("a\nb"), /a.b/s.test("a\nb"));
console.log(first(/a.c/, "abc"));

// The alternation is ordered: the FIRST alternative that matches at the
// earliest position wins, so `a` beats `ab` even though `ab` is longer.
console.log(first(/a|ab/, "ab"));
console.log(first(/x|yy|zzz/, "azzzb"));

console.log(first(/(ab)\1/, "xabab"));
console.log(first(/(?:ab)+/, "ababab"));
console.log(/(a)?\1b/.test("b"));

console.log(first(/a(?=b)/, "ab"));
console.log(first(/a(?=b)/, "ac"));
console.log(first(/a(?!b)/, "ab"));
console.log(first(/a(?!b)/, "ac"));

console.log(first(/ABC/i, "xxabcxx"));
console.log(first(/[a-z]+/i, " ABC "));

const capt = /(\w+)@(\w+)\.com/.exec("mail: bob@site.com");
console.log(capt[1], capt[2], capt.index, capt.length);

const named = /(?<h>\d\d):(?<mi>\d\d)/.exec("at 09:30");
console.log(named.groups.h, named.groups.mi);

// A repeated group's captures are cleared at the start of every turn
// (22.2.2.5.1 step 4), so the second turn does not inherit the first's.
const repeated = /(?:(a)|b)*/.exec("ab");
console.log(repeated[0], repeated[1]);

// Sticky: one attempt, at `lastIndex`, and the cursor moves on a hit and
// resets on a miss.
const sticky = /a/y;
console.log(sticky.test("ba"), sticky.lastIndex);
sticky.lastIndex = 1;
console.log(sticky.test("ba"), sticky.lastIndex);
