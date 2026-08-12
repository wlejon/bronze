// `Object.keys` — and `values` and `entries`, which are one walk over it — on
// the object kinds that are not a plain object or an array.
//
// All five refused with `Object.keys is only supported on plain objects and
// arrays`, which names bronze's COVERAGE where the question is about the
// receiver's storage. For every kind below the language's answer is derivable
// and complete, so a refusal was the wrong shape of answer rather than a
// missing one:
//
//   A Map's and a Set's entries are internal slots reached through `get` and
//   `add`, never properties. A RegExp's `lastIndex` IS an own property but
//   22.2.6.9 defines it non-enumerable. An ArrayBuffer's `byteLength` and a
//   DataView's `byteLength`, `buffer` and `byteOffset` are accessors on their
//   prototypes, so they are own properties of nothing. Each of those has no own
//   enumerable string-keyed property at all, and `[]` is the whole answer.
//
//   A TYPED ARRAY is the exception, and it is why the group could not be
//   answered with one comment: 10.4.5.3 gives an integer-indexed element
//   `enumerable: true`, so its indices really are own enumerable keys and
//   `Object.keys(new Uint8Array(3))` is `["0","1","2"]`. Answering `[]` for one
//   would have been a wrong answer wearing the right shape.
//
// A FUNCTION is here for the same reason and pins a distinction worth keeping:
// `Object.keys` of one is COMPLETE — its own `length`, `name` and `prototype`
// are all non-enumerable (10.2.4, 20.2.4), so the statics are the entire answer
// — while `Object.getOwnPropertyNames` of the same function is still refused by
// name, because that member wants those three and bronze stores none of them.
// Two members, two answers, and the difference is the enumerable filter rather
// than how much has been built.
//
// What each line pins, from 20.1.2.17 (Object.keys -> 7.3.23
// EnumerableOwnProperties with key-of-type-String), 10.4.5.3, 22.2.6.9, 10.2.4
// and 15.7.14:
//
// 1. The five kinds with nothing own and enumerable, one per line so a
//    regression names which one.
// 2. `values` and `entries` agree with `keys`, because they are defined as a
//    loop over it.
// 3. A typed array reports its indices, in ascending order, and `values` reads
//    the elements back through them — the case that proves the group was not
//    answered by a blanket `[]`.
// 4. A function reports its statics and nothing else. `Object.keys(g)` of a
//    function that was never given one is empty rather than an error, and a
//    CLASS's static method is absent because 15.7.14 defines a method
//    non-enumerable — the same rule that keeps class methods out of every other
//    enumeration.
// 5. A RegExp's `lastIndex` is writable and readable and still not a key, which
//    is the one own property in this file that exists and is filtered out.

// 1.
console.log(Object.keys(new Map([["a", 1]])).length);
console.log(Object.keys(new Set([1, 2])).length);
console.log(Object.keys(new ArrayBuffer(8)).length);
console.log(Object.keys(new DataView(new ArrayBuffer(4))).length);
console.log(Object.keys(/ab+/g).length);

// 2.
console.log(Object.values(new Map([["a", 1]])).length, JSON.stringify(Object.entries(new Set([1]))));
console.log(Object.values(/x/).length, JSON.stringify(Object.entries(new DataView(new ArrayBuffer(2)))));

// 3.
const v = new Uint8Array(3);
v[0] = 7;
console.log(Object.keys(v).join(","));
console.log(Object.values(v).join(","));
console.log(JSON.stringify(Object.entries(v)));
console.log(Object.keys(new Uint8Array(0)).length);

// 4.
function g() {}
console.log(Object.keys(g).length);

function f() {}
f.a = 1;
f.b = 2;
console.log(Object.keys(f).join(","), Object.values(f).join(","));

class C {
  static m() {}
}
C.tag = "t";
console.log(Object.keys(C).join(","));

// 5.
const re = /ab/g;
re.lastIndex = 1;
console.log(Object.keys(re).length, re.lastIndex);
