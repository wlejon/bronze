// The SOURCE of an object spread, which ToObject settles: 7.3.25
// CopyDataProperties step 3 for `{ ...src }`, and 20.1.2.1 step 3.b for
// `Object.assign`. `cases/object_own_keys_primitive` pins the TARGET side of
// the same clause; this is the other one.
//
// What a value contributes is what its box's own ENUMERABLE properties are:
//
// - `null` and `undefined` contribute nothing and do not raise (step 3.a),
//   which is what `{ ...maybeOptions }` relies on.
// - A STRING contributes its index properties, because 10.4.3.5 makes each
//   index of the box an own enumerable property. `length` is NOT among them:
//   10.4.3.4 defines it non-enumerable, so `{ ..."ab" }` has two properties
//   and not three.
// - A number, a boolean or a symbol contributes NOTHING. Its box has no own
//   property at all, so an empty result is the complete answer rather than an
//   approximation of one.
//
// bronze refused every primitive but `null` and `undefined` here, which turned
// three correct empty answers and one two-property answer into a process
// death. The empty ones are what made the refusal tempting -- a quiet `{}` is
// the shape of bug that hides longest -- but 20.1.2.1 is explicit, and the box
// that would have to be built to prove it has no own property to find. The
// box for a number is buildable now (`cases/number_prototype_chain`) and is
// still not built here, because CopyDataProperties reads own keys and a Number
// object has none.
//
// The last line is the seam that is NOT this: spreading a string into an ARRAY
// is the iterator walk that `for-of` uses, not CopyDataProperties, and it was
// already right.

console.log(JSON.stringify(Object.assign({}, "ab")));
console.log(JSON.stringify({ ..."ab" }));
console.log(JSON.stringify({ ...""}));

console.log(JSON.stringify(Object.assign({}, 5)));
console.log(JSON.stringify(Object.assign({}, true, false)));
console.log(JSON.stringify(Object.assign({}, Symbol("s"))));
console.log(JSON.stringify(Object.assign({}, null, undefined)));
console.log(JSON.stringify({ ...5, ...true, ...null, ...undefined, ...Symbol("s") }));

// `length` is an own property of the box and is not enumerable, so it is the
// one own key CopyDataProperties filters out.
const three = Object.assign({}, "abc");
console.log(Object.keys(three).join(","), three.length, three[2]);

// A later source overwrites an earlier one, on the ordinary rule: this is a
// property write into the object being built, not a merge.
console.log(JSON.stringify({ ..."ab", ...{ "1": "Z" } }));
console.log(JSON.stringify({ ...{ "1": "Z" }, ..."ab" }));
console.log(Object.assign({ "0": "x" }, "ab")[0]);

// A string source contributes nothing a symbol key could collide with, and a
// symbol source contributes nothing at all -- including no description.
const withSym = Object.assign({}, Symbol("tag"));
console.log(Object.keys(withSym).length, Object.getOwnPropertySymbols(withSym).length);

// The iterator walk, which is a different operation with a similar spelling.
console.log(JSON.stringify([..."ab"]));
