// 8.4.5 NamedEvaluation reaches anonymous CLASS expressions as it does
// anonymous functions: 15.7.15 ClassDefinitionEvaluation receives the name
// the surrounding syntax supplies and gives it to the constructor, so
// `var K = class {}` has `K.name === "K"`. The name is only the class's
// `name`; unlike a class that writes its own name, no binding of it is
// created inside the class body.

var V = class {};
let L = class {};
const C = class extends L {};
console.log(V.name, L.name, C.name);

let assigned;
assigned = class {};
console.log(assigned.name);

const obj = { prop: class {}, "quoted key": class {} };
console.log(obj.prop.name, obj["quoted key"].name);

function withDefault(k = class {}) { return k.name; }
console.log(withDefault());

const { destructured = class {} } = {};
console.log(destructured.name);

// A class that wrote its own name keeps it.
const Outer = class Inner {};
console.log(Outer.name);

// A static `name` member is defined after the name, and replaces it.
const WithStatic = class { static name() { return "method"; } };
console.log(typeof WithStatic.name, WithStatic.name());

// Not NamedEvaluation positions: a member target, a comma expression.
const holder = {};
holder.k = class {};
console.log(JSON.stringify(holder.k.name), JSON.stringify((0, class {}).name));

// The inferred name binds nothing inside the class: `Q` in the method is the
// outer binding, which is reassigned.
let Q = class { static self() { return Q; } };
const saved = Q;
Q = 1;
console.log(saved.name, saved.self());

// The name is non-enumerable, non-writable and configurable.
const d = Object.getOwnPropertyDescriptor(V, "name");
console.log(d.value, d.enumerable, d.writable, d.configurable);
