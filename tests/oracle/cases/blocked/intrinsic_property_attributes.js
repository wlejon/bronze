// BLOCKED: bronze reports `writable: true, configurable: true` for intrinsic
// properties ECMA-262 defines as neither.
//
// The reason is storage, not coverage, and it is uniform across every intrinsic
// in the runtime. A property created by a SHAPE TRANSITION carries exactly one
// attribute — `enumerable`, which is part of the transition key — and
// `PropertyInfo` fixes `writable` and `configurable` true for every property in
// a shape chain (src/runtime/shape.h). The only storage that records the other
// two is DICTIONARY mode, which `Object.defineProperty` and `Object.freeze`
// move an object into. So an intrinsic can be accurate about `enumerable` and
// cannot yet be accurate about the rest, and the fix is not a table entry
// anywhere: it is either a per-property attribute in the transition tree, or
// building the intrinsics through the dictionary path and paying for it on
// every read of `Object.prototype`.
//
// `enumerable` is the half a program usually sees, and bronze is right about
// it: `Object.keys(Number)` is `[]` and a for-in over a number visits nothing
// (`cases/number_prototype_chain`). What is pinned here is the half a program
// sees only by asking, or by writing.
//
// The receiver here is `Object`, which is a plain namespace object. The same
// divergence applies to `Number`'s and `Symbol`'s statics and cannot be spelled
// against them: `Object.getOwnPropertyDescriptor` of a FUNCTION is a named
// refusal (its own keys come from a `prototype` slot, two header fields and a
// side object), so the question cannot be asked of a constructor at all yet.
//
// What this pins, from 20.1.2.19 (Object.prototype's attributes), 6.2.6.4
// (FromPropertyDescriptor), 10.1.9.2 (OrdinarySetWithOwnDescriptor) and
// 13.15.2 (a sloppy assignment discards a refusal):
//
// 1. `Object.prototype` is { writable: false, enumerable: false,
//    configurable: false } — bronze answers true for the first and third.
// 2. Assigning to it in sloppy code is a silent no-op, so the intrinsic
//    survives. In bronze the assignment lands, which is the same divergence
//    doing real damage rather than reporting itself.
// 3. 21.1.3's METHODS are { writable: true, enumerable: false, configurable:
//    true }, so those bronze already gets right — the line is here to keep the
//    two apart, and to fail loudly if a fix over-corrects.

const d = Object.getOwnPropertyDescriptor(Object, "prototype");
console.log(d.value === Object.prototype, d.writable, d.enumerable, d.configurable);

const m = Object.getOwnPropertyDescriptor(Number.prototype, "toFixed");
console.log(typeof m.value, m.writable, m.enumerable, m.configurable);

const before = Object.prototype;
Object.prototype = 5;
console.log(Object.prototype === before);
