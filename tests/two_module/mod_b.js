// Module B of the two-module test: a separate compiled object, entered after
// module A, sharing one runtime and one heap with it.
//
// B knows A only through globalThis and the property names B spells itself. B
// numbers those names 0..n-1 in its own key pool, and its numbering is NOT A's
// — the two pools overlap on the names both modules mention and diverge on the
// rest. Interning at module init is what collapses the overlap onto one id.

const label = "B";
function tag(s) { return label + " " + s; }

const registry = globalThis.registry;
registry.count = registry.count + 41;
registry.items.push("beta");
registry.betaOnly = true;
console.log(tag("count=" + registry.count + " items=" + registry.items.join(",")));
console.log(tag("alphaOnly=" + registry.alphaOnly + " betaOnly=" + registry.betaOnly));

// A class extending a class module A published: the prototype chain crosses the
// module boundary, and `instanceof` compares against A's constructor object.
// The heritage is a plain binding because bronze's parser takes an identifier
// there and not the LeftHandSideExpression 15.7.1 allows; `Shape` here is still
// the constructor object module A built, which is what the test is about.
const Shape = globalThis.Shape;
class Circle extends Shape {
  constructor(r) { super("circle"); this.radius = r; }
  describe() { return super.describe() + "+r" + this.radius; }
}
const c = new Circle(3);
console.log(tag(c.describe() + " isShape=" + (c instanceof Shape)));

// A's closures over A's module scope, written through A's setter from here.
globalThis.writeSecret("b1");
console.log(tag("secret=" + globalThis.readSecret() + " bump=" + globalThis.bump(1)));

// A throw compiled into B, caught by the `try` compiled into A.
function boom() { throw new TypeError("thrown in B"); }
console.log(tag(globalThis.callGuarded(boom)));
console.log(tag(globalThis.callGuarded(function () { return "no throw"; })));

// Repeated reads of `globalThis` and of B's own `tag` declaration, so that
// under BRONZE_GC_STRESS every one of them crosses a collection. `globalThis`
// is served from B's module-local global cache and `tag` from B's module-local
// function-singleton table, so a cell the collector failed to forward shows up
// here as a stale pointer rather than as a quietly missing root.
let acc = 0;
for (let i = 0; i < 8; i++) { acc += globalThis.bump(1); }
console.log(tag("acc=" + acc + " count=" + globalThis.registry.count));
