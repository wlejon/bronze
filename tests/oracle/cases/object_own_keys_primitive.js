// The `Object` members whose step 1 is ToObject, asked about a PRIMITIVE — the
// receiver 7.1.18 succeeds on for everything except `null` and `undefined`.
//
// `hasOwn`, `getOwnPropertyNames`, `getOwnPropertyDescriptor`,
// `getOwnPropertyDescriptors`, `keys`, `values`, `entries` and `assign` all
// threw `TypeError: ... called on a value that is not an object` for a string,
// a number and a boolean. That sentence is true of `null` and `undefined` and
// of nothing else these members can be handed: ToObject boxes every other
// primitive, and the box has own keys the language names exactly.
//
// The box is not built for the seven that only READ own keys, which is the
// arrangement `Object.getPrototypeOf` of a primitive already uses. It buys more
// than a skipped allocation here: neither a Number object nor a Symbol object
// has an own property of any kind, so the empty answer is COMPLETE rather than
// the one bronze can reach — and a Symbol object is one bronze cannot make at
// all, since 20.4.3 gives `Symbol.prototype` no [[SymbolData]] slot. `assign` is
// the exception and has to build its box, because the box is what it returns;
// it is also why a symbol is the one primitive `assign` still refuses.
//
// This is about the PRIMITIVE and not about the exotic object: `Object.keys(new
// String("ab"))` is still refused by name, because reporting a String OBJECT's
// own keys means materialising index keys inside a walk that is handed a raw
// header and may not allocate (`cases/blocked/string_object_own_keys`). A
// primitive needs no such walk — the characters are right there.
//
// What each group pins, from 7.1.18 (ToObject), 10.4.3.3 (a String exotic
// object's OwnPropertyKeys), 10.4.3.4 (StringCreate's `length`), 10.4.3.5
// (StringGetOwnProperty), 6.2.6.4 (FromPropertyDescriptor) and 20.1.2.1
// (Object.assign):
//
// 1. A string's own keys are its indices and `length`. An index inside the
//    length is one; `"01"` is not, because 10.4.3.5 takes only a CANONICAL
//    numeric string; a member off `String.prototype` is not, because it is
//    inherited.
// 2. `getOwnPropertyNames` reports the indices ascending and THEN `length` —
//    10.4.3.3's order, not a creation order — where `keys` drops `length`,
//    since 10.4.3.4 defines it non-enumerable and 10.4.3.5 makes every index
//    enumerable. That one attribute is the whole difference between the two
//    lines, which is why both are here.
// 3. The descriptors: non-writable and non-configurable for both kinds of own
//    key, so no program can shadow an index or delete one.
// 4. A number and a boolean box to an object with NO own property, so every one
//    of these answers is the empty one — `false`, `[]`, `undefined`, `{}`.
// 5. `null` and `undefined` still throw, and that TypeError is ToObject's own.
//    Its message is unchanged and stays true: these two really are not objects.
// 6. `assign` returns the BOX, so `Object.assign("ab", ...)` is an object that
//    still indexes and measures like the string it wraps. A source key that
//    collides with one of those own keys is the TypeError `Set(to, key, v,
//    true)` raises for a non-writable property (step 3.c.iii) — never a shadow
//    property, which is what a plain shape write would have left behind and
//    which no read could ever reach.

function message(fn) {
  try {
    fn();
    return "no throw";
  } catch (e) {
    return e.name + ": " + e.message;
  }
}

// 1. A string's own keys.
console.log(Object.hasOwn("ab", 0), Object.hasOwn("ab", 1), Object.hasOwn("ab", 2));
console.log(Object.hasOwn("ab", "length"), Object.hasOwn("ab", "01"), Object.hasOwn("ab", "toUpperCase"));

// 2. The two key lists, and the one attribute that separates them.
console.log(Object.getOwnPropertyNames("ab").join(","));
console.log(Object.keys("ab").join(","));
console.log(Object.getOwnPropertyNames("").join(","));
console.log(Object.values("ab").join(","), JSON.stringify(Object.entries("ab")));

// 3. The descriptors.
const d = Object.getOwnPropertyDescriptor("ab", 0);
console.log(d.value, d.writable, d.enumerable, d.configurable);
const dl = Object.getOwnPropertyDescriptor("ab", "length");
console.log(dl.value, dl.writable, dl.enumerable, dl.configurable);
console.log(Object.getOwnPropertyDescriptor("ab", 5));
console.log(JSON.stringify(Object.getOwnPropertyDescriptors("ab")));

// 4. A number and a boolean: the empty answer, every time.
console.log(Object.hasOwn(5, "toFixed"), Object.hasOwn(true, "valueOf"));
console.log(Object.getOwnPropertyNames(5).length, Object.getOwnPropertyNames(true).length);
console.log(Object.keys(5).length, Object.keys(false).length);
console.log(Object.getOwnPropertyDescriptor(5, "toFixed"), Object.getOwnPropertyDescriptor(false, "x"));
console.log(JSON.stringify(Object.getOwnPropertyDescriptors(7)));

// 5. ToObject's own two failures.
console.log(message(() => Object.hasOwn(null, "a")));
console.log(message(() => Object.getOwnPropertyNames(undefined)));
console.log(message(() => Object.getOwnPropertyDescriptor(null, "a")));
console.log(message(() => Object.keys(undefined)));

// 6. `assign` returns the box.
const boxed = Object.assign("ab", { tag: "x" });
console.log(typeof boxed, boxed.tag, boxed.length, boxed[0], boxed.valueOf());
const flag = Object.assign(false, { tag: "y" });
console.log(typeof flag, flag.tag, flag.valueOf());
console.log(message(() => Object.assign("ab", { "0": "z" })));
console.log(message(() => Object.assign("ab", ["z"])));
console.log(message(() => Object.assign(null, {})));
