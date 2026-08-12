// A HOLE — the state `delete a[1]` leaves an array index in — and the two
// different questions the rest of the language asks about one.
//
// A hole is only reachable through `delete`: the other way to make one is a
// sparse write, which is a named hard error, so every array bronze builds is
// otherwise dense from 0 to `length`. Deleting an element changes that,
// and the change is not local to the element read: a hole is NOT AN OWN
// PROPERTY, and every operation defined over own keys has to agree about it,
// while every operation defined over `Get` has to keep reading it as
// `undefined`. Reading a hole and asking whether a hole is there are
// different questions with different answers, and this case pins the split
// method by method, because getting it wrong is silent in every direction.
//
// What this pins, from ECMA-262 10.4.2.1, 7.3.12 (HasProperty), 23.1.3 and
// 14.7.5.6:
//
// 1. `length` does not move, the index stops being an own key, and the read
//    answers `undefined`. So `a[1]` and `1 in a` disagree, which no dense
//    array can make happen.
// 2. Everything that enumerates OWN KEYS agrees on the same set:
//    `Object.keys`, `for-in`, and object spread. `for-of` does not — the
//    array iterator is defined with `Get` over `0..length`, so it yields
//    `undefined` for a hole and array spread produces a DENSE array from a
//    sparse one.
// 3. The searches defined with HasProperty skip a hole: `indexOf(undefined)`
//    is -1 on an array full of holes. The ones defined with `Get` do not:
//    `includes(undefined)` is true and `findIndex` finds the hole at 1. That
//    pair is the whole distinction in two lines.
// 4. A callback method never visits a hole — `forEach` on five slots with two
//    holes calls back three times, with the ORIGINAL indices, not renumbered.
// 5. A producer either preserves the absence or densifies it, and which is
//    which is fixed by the spec rather than by convenience: `map`, `slice`
//    and `concat` keep the holes in place (and `concat` therefore keeps its
//    length), while `filter` drops them because it drops anything it does not
//    emit.
// 6. `reduce` with no initial value seeds from the first PRESENT element, not
//    from index 0.
// 7. `console.log` prints a run of holes as one `<n empty items>` entry, and
//    the singular form for a run of one — the inspect format, first
//    reachable here.
const a = [1, 2, 3, 4, 5];
delete a[1];
delete a[3];
console.log(a);
console.log(a.length);
console.log(1 in a);
console.log(2 in a);
console.log(a[1]);
console.log(Object.keys(a).join(","));
let ks = "";
for (const k in a) {
  ks = ks + k;
}
console.log(ks);
let vs = "";
for (const v of a) {
  vs = vs + typeof v + ",";
}
console.log(vs);
console.log(a.join("-"));
console.log({ ...a });
console.log([...a]);

console.log(a.indexOf(undefined));
console.log(a.lastIndexOf(undefined));
console.log(a.includes(undefined));
console.log(a.indexOf(3));
console.log(a.find((v) => v === undefined));
console.log(a.findIndex((v) => v === undefined));

let seen = "";
a.forEach((v, i) => {
  seen = seen + i + ":" + v + ";";
});
console.log(seen);
console.log(a.map((v) => v * 2));
console.log(a.filter((v) => v > 1));
console.log(a.some((v) => v === undefined));
console.log(a.every((v) => v > 0));
console.log(a.reduce((s, v) => s + v));

const seedHole = [0, 1, 2];
delete seedHole[0];
console.log(seedHole.reduce((s, v) => s + "," + v));

console.log(a.slice(0, 3));
const cc = a.concat([7]);
console.log(cc.length);
console.log(cc);

const runs = [1, 2, 3, 4];
delete runs[1];
delete runs[2];
console.log(runs);

const r = [1, 2, 3];
delete r[0];
r.reverse();
console.log(r);
