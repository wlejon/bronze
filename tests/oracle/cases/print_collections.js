// `console.log` of a Map and of a Set, in the pinned inspect format.
//
// The format follows `cases/print_containers`, which pins arrays, objects,
// circular references and typed arrays: `Float32Array(3) [ 0, 0, 0 ]` is the
// precedent for the `Ctor(size)` prefix, and the single-quoted string and the
// spaces inside the braces are that case's rules too. A Map's entries carry
// ` => ` between key and value, which is the one piece of spelling a Map adds;
// a Set prints its elements as a list, having no second half to separate. An
// EMPTY collection still prints its constructor and its size, which is what
// distinguishes it in output from the `{}` of an object with no properties.
//
// It is here rather than in `cases/map_and_set` on this suite's rule that a
// case which grows to cover everything stops naming what it is for: that case
// pins the data structure — key identity, NaN keys, -0 normalisation,
// insertion order — through `size`, `get` and `has`, and never once looks at
// the collection itself. This case looks at it.
//
// A Map and a Set are one layout in the runtime, told apart by the header
// flag, and `inspect.cpp` has an arm for each. What it no longer has is a
// `default:` that casts an unrecognised heap kind to an ObjectHeader and reads
// a shape word that is not there: printing a Map used to be exit 139 with
// nothing on stdout, which is the failure the house rules rank below every
// wrong answer, since a crash names nothing and names it too late.

const m = new Map();
m.set('a', 1);
m.set('b', 2);
console.log(m);
console.log(new Map());

const s = new Set();
s.add(1);
s.add(2);
console.log(s);
console.log(new Set());
