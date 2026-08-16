// The `d` flag's edges, beside `regexp_match_indices`: what a pair array says
// for a group that did not participate, and where its numbers are measured
// from.
//
// 22.2.7.8 builds the pair array over the SAME capture list `exec` builds the
// match array over, so `indices.length` and the match's own `length` are one
// count and a non-participating group is `undefined` in both. That is the first
// half.
//
// The second half is the one a reader can get wrong: 22.2.7.8 GetMatchIndexPair
// reads [[StartIndex]] and [[EndIndex]] of a Match Record, and those are
// positions in the WHOLE input — not offsets from where the attempt began. So a
// `g` or `y` pattern whose `lastIndex` has moved reports indices that agree
// with `m.index`, and a case that only ever matched at 0 could not tell the two
// readings apart.
//
// `matchAll` is here because it does not call `exec` on the receiver: 22.1.3.16
// makes its own copy of the pattern with `g` added, and the copy has to keep
// `d` or every result it yields would silently lose `indices`.

const opt = /a(b)?c/d.exec("ac");
console.log(opt.indices.length, opt.length);
console.log(opt.indices[1] === undefined, opt[1] === undefined);
console.log(opt.indices[0].join(","));

const named = /(?<x>a)/d.exec("a");
console.log(named.indices.groups.x.join(","));

const g = /b/dg;
g.lastIndex = 2;
const gm = g.exec("abab");
console.log(gm.index, gm.indices[0].join(","), g.lastIndex);

const y = /ab/dy;
y.lastIndex = 2;
const ym = y.exec("abab");
console.log(ym.index, ym.indices[0].join(","));

console.log(new RegExp("a", "gd").flags, new RegExp("a", "gd").hasIndices);

for (const mm of "abab".matchAll(/(b)/dg)) {
    console.log(mm.index, mm.indices[0].join(","), mm.indices[1].join(","));
}

console.log("ab".replace(/(a)/d, (m, p1, off) => p1 + off));
