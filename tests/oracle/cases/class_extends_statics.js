// STATIC members of a native constructor, reached through `extends`.
//
// 15.7.14 step 6 makes the base constructor the derived one's [[Prototype]],
// so `MyArr.of` is `Array.of` found by an ordinary prototype walk — the same
// walk that makes `D.s` work for `class Base { static s() {} }`. Nothing about
// that is special to natives in the language, and the point of this case is
// that nothing is special about it in bronze either.
//
// It was, for a while: several intrinsics answer their statics BESIDE the
// value, off a table keyed on the constructor's code pointer, because an
// interned function singleton had no property object worth building. A
// subclass is a different function object with a different code pointer, so
// the table never fired and the walk found nothing — `typeof MyArr.of` read
// `"undefined"` where the language says `"function"`. The statics are now
// written into the base's property box at the `extends` link, which is what
// `Promise` had all along; the identity lines below pin that this is ONE
// function reached two ways and not a copy.
//
// The second half is what `of` and `from` then DO. 23.1.2.2 step 4 and
// 23.1.2.1 steps 5.b and 7.b build the result by constructing `this`, so
// reaching them is only half the answer — a reachable `MyArr.of` that returned
// a plain Array would be the silent wrong type. Both argument shapes are here
// because the two paths hand the base different things: the iterator path
// constructs with NO argument and sets `length` at the end, the array-like
// path constructs with the length up front.
//
// `Map.groupBy` goes the other way and that is also the language: 24.1.2.1
// step 2 is `Construct(%Map%)`, naming the intrinsic rather than `this`, so a
// subclass's `groupBy` returns a plain Map. Pinned as the correct answer, not
// as a divergence.

class MyArr extends Array {}
class MyMap extends Map {}
class MySet extends Set {}
class MyP extends Promise {}

console.log(typeof MyArr.of, typeof MyArr.from, typeof MyArr.isArray);
console.log(MyArr.of === Array.of, MyArr.from === Array.from, MyArr.isArray === Array.isArray);
console.log(typeof MyMap.groupBy, MyMap.groupBy === Map.groupBy);
console.log(typeof MyP.resolve, typeof MyP.all, MyP.resolve === Promise.resolve);
// 24.2.2 defines no `Set.groupBy`, so `undefined` here is the language's
// answer and not a member that went missing with the others.
console.log(typeof MySet.groupBy);

// Own properties of an intrinsic are non-enumerable (23.1.2, 24.1.2), and
// realizing them must not have made them visible to a key walk.
console.log(Object.keys(Array).length, Object.keys(MyArr).length, Object.keys(Map).length);

class Deeper extends MyArr {}
console.log(typeof Deeper.of, typeof Deeper.isArray, Deeper.of === Array.of);

// A static DEFINED on the subclass shadows the inherited one, which is the
// ordinary rule and the reason this is a chain and not a merge.
class Shadow extends Array {
  static isArray(x) {
    return "mine:" + Array.isArray(x);
  }
}
console.log(Shadow.isArray([]), Array.isArray([]), MyArr.isArray([]));

// 23.1.2.2: Construct(C, « len »), then the elements, then `length`.
const a = MyArr.of(1, 2, 3);
console.log(a.length, a.join(","), a instanceof MyArr, Array.isArray(a));
// 23.1.2.1 step 7.b, the array-like path: Construct(C, « len »).
const b = MyArr.from([4, 5, 6]);
console.log(b.length, b.join(","), b instanceof MyArr);
// Step 5.b, the iterator path: Construct(C) with no argument at all, because
// the count is not known until the iterator is exhausted.
const c = MyArr.from(new Set([7, 8, 8]));
console.log(c.length, c.join(","), c instanceof MyArr);
const d = MyArr.from({ length: 2, 0: "x", 1: "y" });
console.log(d.length, d.join(","), d instanceof MyArr);
const e = MyArr.from([1, 2, 3], (x) => x * 3);
console.log(e.join(","), e instanceof MyArr);
console.log(MyArr.of().length, MyArr.from([]).length, MyArr.from([]) instanceof MyArr);

// What comes out is a real subclass instance, so @@species carries from it.
console.log(a.map((x) => x + 1) instanceof MyArr, a.map((x) => x + 1).join(","));

// The plain forms are untouched, and a DETACHED one has no `this` to build
// through — IsConstructor(undefined) is false, which is step 4's other arm.
console.log(Array.of(1, 2, 3).join(","), Array.of(1) instanceof MyArr, Array.isArray(Array.of(1)));
const ofFn = Array.of;
console.log(ofFn(1, 2).join(","), Array.isArray(ofFn(1, 2)));

const g = MyMap.groupBy([1, 2, 3, 4], (x) => (x % 2 === 0 ? "even" : "odd"));
console.log(g instanceof Map, g instanceof MyMap, g.size, g.get("even").join(","), g.get("odd").join(","));

// 27.2.4.7 and the combinators are specified over `this`, so these DO build
// the subclass — the same statics-through-extends walk, a different rule about
// what the member then constructs.
console.log(MyP.resolve(1) instanceof MyP, MyP.all([]) instanceof MyP);
