// BLOCKED: bronze uses ONE prototype object where ECMA-262 has two.
//
// 24.1.5.2.2 gives %MapIteratorPrototype% an @@toStringTag of "Map Iterator"
// and 24.2.5.2.2 gives %SetIteratorPrototype% "Set Iterator". bronze's
// `IteratorProto::Map` (runtime/iterator.h) stands in for both, because what
// the kinds are kept apart for is the BRAND a `next` checks its receiver with,
// and a Map iterator and a Set iterator need only one brand between them.
//
// So no value is right for both receivers, and the tag is left off that
// prototype rather than guessed — an object with no tag reads "[object Object]",
// which is a missing answer where "Map Iterator" on a Set's iterator would be
// a wrong one. The three prototypes that stand for exactly one of ECMA-262's do
// carry theirs (cases/to_string_tag_builtins pins %GeneratorPrototype%'s).
//
// Unblocking this means splitting `IteratorProto::Map` into a Map kind and a
// Set kind, each with its own prototype object.
const ts = Object.prototype.toString;
console.log(ts.call(new Map().entries()));
console.log(ts.call(new Set().values()));
