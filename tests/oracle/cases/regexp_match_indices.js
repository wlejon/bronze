// The RegExp `d` flag: 22.2.6.6 hasIndices, and the `indices` array 22.2.7.2
// attaches to a match.
//
// 22.2.6.4's `flags` getter spells the eight flags in a fixed order —
// d, g, i, m, s, u, v, y — so `/a/dgimsy`.flags is "dgimsy" whatever order the
// source wrote them in, and `hasIndices` (22.2.6.6) is the accessor that reads
// the `d` bit back. The last line pins the order, which is the half of this
// feature that has nothing to do with matching.
//
// The rest is 22.2.7.2 RegExpBuiltinExec step 34 and 22.2.7.8
// MakeMatchIndicesIndexPairArray. With `d` set, the result array carries an
// `indices` property holding one entry per capture — `[start, end]` as a
// two-element array, half-open like every other range in the language — and
// `indices.length` equals the match array's own length because both count the
// same captures. A group that did not participate has `undefined` there rather
// than a pair, which is the same distinction `m[2] === undefined` draws and the
// reason the alternation line is here.
//
// `indices.groups` is `undefined` when the pattern has no named groups, and
// otherwise an object from OrdinaryObjectCreate(NULL) — no prototype, exactly
// as `groups` itself is — keyed by group name. Without `d`, `indices` is not
// there at all.
//
// Nothing here is a matching question. The capture extents the pair array is
// built from are the ones the matcher already records to cut the captures out
// of, so `d` changes what `exec` REPORTS and never what it finds —
// `regexp_match_indices_edges` pins the other half of that, which is that the
// positions are absolute and so survive a `lastIndex` the cursor has moved.
const re = /a(b)/d;
console.log(re.flags, re.hasIndices, /a(b)/.hasIndices);

const m = re.exec("xab");
console.log(m.index, m[0], m[1]);
console.log(m.indices.length, m.length);
console.log(m.indices[0].join(","), m.indices[1].join(","));
console.log(m.indices.groups === undefined);

const named = /(?<tail>b)/d.exec("ab");
console.log(named.indices[0].join(","), named.indices[1].join(","));
console.log(named.indices.groups.tail.join(","));
console.log(Object.getPrototypeOf(named.indices.groups) === null);

const alt = /(a)|(b)/d.exec("b");
console.log(alt.indices.length, alt.indices[0].join(","));
console.log(alt.indices[1] === undefined, alt.indices[2].join(","));

console.log(/a/g.exec("a").indices === undefined);
console.log(/a/dgimsy.flags);
