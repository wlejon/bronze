// Intrinsic properties carry the attributes ECMA-262 assigns them, not the
// shape default (20.1.2.19, 6.2.6.4, 10.1.9.2, 13.15.2).
//
// A property born of a shape transition carries per-property attributes now,
// so an intrinsic can be { writable: false, configurable: false } without
// moving its holder into dictionary mode. What this pins, receiver by
// receiver:
//
// 1. `Object.prototype` is { writable: false, enumerable: false,
//    configurable: false } (20.1.2.19), and `getOwnPropertyDescriptor`
//    reports exactly that (6.2.6.4).
// 2. Assigning to it in sloppy code is a silent no-op (10.1.9.2 refuses the
//    write, 13.15.2 discards the refusal), so the intrinsic survives the
//    assignment — the same fact observed by writing rather than by asking.
// 3. 21.1.3's METHODS are { writable: true, enumerable: false,
//    configurable: true }, deliberately different on two of the three — the
//    line is here to keep the two attribute sets apart, and to fail loudly if
//    a fix over-corrects.

const d = Object.getOwnPropertyDescriptor(Object, "prototype");
console.log(d.value === Object.prototype, d.writable, d.enumerable, d.configurable);

const m = Object.getOwnPropertyDescriptor(Number.prototype, "toFixed");
console.log(typeof m.value, m.writable, m.enumerable, m.configurable);

const before = Object.prototype;
Object.prototype = 5;
console.log(Object.prototype === before);
