// %MapIteratorPrototype% and %SetIteratorPrototype% are two objects, each
// with its own @@toStringTag (ECMA-262 24.1.5.2.2, 24.2.5.2.2).
//
// 24.1.5.2.2 gives a Map iterator's prototype the tag "Map Iterator" and
// 24.2.5.2.2 gives a Set iterator's "Set Iterator". The tag is the one
// observable fact that forces the prototypes apart: the BRAND a `next` checks
// its receiver with could share one object, but no single tag value is right
// for both receivers. This case pins that a Map's iterator and a Set's
// iterator answer differently, which only two distinct prototype objects can
// do.
const ts = Object.prototype.toString;
console.log(ts.call(new Map().entries()));
console.log(ts.call(new Set().values()));
