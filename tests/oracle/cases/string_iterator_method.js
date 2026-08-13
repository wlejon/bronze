// String.prototype[Symbol.iterator] (ECMA-262 22.1.3.36) and the
// StringIterator it hands back (22.1.5), stepping by CODE POINT.
//
// Derived from ECMA-262:
//
// 1. 22.1.3.36 is an own property of `String.prototype`, so the method read
//    off a primitive is the SAME function object the prototype holds — the
//    ordinary walk, not a copy per string.
// 2. 22.1.5.1 steps by code point: a surrogate pair is ONE iteration whose
//    value has length 2, which is why the emoji built from the pair
//    (0xD83D, 0xDE00) yields piece lengths "2,1" and the manual `next` walk
//    sees the pair first and "a" second.
// 3. 27.1.2.1: an iterator is its own iterable.
// 4. 22.1.5.1.2: %StringIteratorPrototype%'s @@toStringTag is
//    "String Iterator".
const s = 'hi';
console.log(typeof s[Symbol.iterator]);
const it = s[Symbol.iterator]();
console.log(it.next().value, it.next().value, it.next().done);
console.log(s[Symbol.iterator] === String.prototype[Symbol.iterator]);
const emoji = String.fromCharCode(55357, 56832) + 'a';
const pieces = [];
for (const ch of emoji) pieces.push(ch.length);
console.log(pieces.join(','));
const eit = emoji[Symbol.iterator]();
console.log(eit.next().value.length);
console.log(eit.next().value);
console.log(eit.next().done);
const sit = s[Symbol.iterator]();
console.log(sit[Symbol.iterator]() === sit);
console.log(Object.prototype.toString.call(s[Symbol.iterator]()));
