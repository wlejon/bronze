// 22.1.3.23 String.prototype.split's LIMIT, and the order the steps run in.
//
// The limit is read at step 4, BEFORE the separator is looked at, and the
// limit-zero exit (step 6) comes ahead of the undefined-separator exit (step
// 7). That ordering is the whole reason `"abc".split(undefined, 0)` is `[]`
// rather than `["abc"]`: no separator means "the whole string as one element",
// but a limit of zero means the array never gets that element.
//
// `undefined` for the limit is 2^32-1 and everything else is ToUint32 (step 4),
// which WRAPS rather than clamping — so -1 is 4294967295 and no limit in
// practice, and 2^32 is 0 and an empty result. Both are pinned, because a
// clamping implementation would agree with the spec on -1 and disagree on 2^32.
//
// The string and RegExp separators are two different algorithms (22.1.3.23 and
// 22.2.6.14 SplitMatcher), so the limit is pinned against both — the RegExp one
// also yields the separator's CAPTURES, and those count against the limit like
// any other element, which the last line pins.
//
// The two empty-subject lines are steps 9 and 10 and they disagree on purpose:
// an empty separator asks for the first min(len, lim) code UNITS, of which an
// empty string has none, while a separator that does not occur yields the
// subject itself.
console.log(JSON.stringify("a1b1c1d".split("1", 2)));
console.log(JSON.stringify("a1b1c1d".split("1")));
console.log(JSON.stringify("a1b1c1d".split("1", 1)));
console.log(JSON.stringify("a1b1c1d".split("1", 0)));
console.log(JSON.stringify("a1b1c1d".split("1", 99)));

console.log(JSON.stringify("a1b1c1d".split(/1/, 2)));
console.log(JSON.stringify("a1b2c".split(/(\d)/, 3)));

console.log(JSON.stringify("".split("")));
console.log(JSON.stringify("".split("x")));

console.log(JSON.stringify("abc".split(undefined, 0)));
console.log(JSON.stringify("abc".split(undefined)));

console.log(JSON.stringify("abc".split("", 2)));
console.log(JSON.stringify("abc".split("", 0)));
console.log(JSON.stringify("abc".split("", 99)));

console.log(JSON.stringify("a,b".split(",", -1)));
console.log(JSON.stringify("a,b".split(",", 4294967296)));
console.log(JSON.stringify("a,b,c".split(",", 2.9)));
