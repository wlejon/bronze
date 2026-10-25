// A TypeScript namespace is an object whose body runs once, and its exports
// are LIVE: an exported variable is the object's property, read and written
// through it everywhere in the body, so a function that changes it changes
// what the outside sees, and an assignment from outside is seen inside.

namespace Counter {
    export let count = 0;
    export function inc() { count++; }
    export function get() { return count; }
    let hidden = 10;
    export function bump() { hidden++; count += hidden; }
}
Counter.inc();
Counter.inc();
console.log("live", Counter.count);
Counter.count = 40;
console.log("written from outside", Counter.get());
Counter.bump();
console.log("bump", Counter.count, "hidden" in Counter);

// Exported functions and classes keep their local binding and are assigned
// to the object after their declaration; exported variables have no local
// binding at all.
namespace Shapes {
    export class Point {
        constructor(public x: number, public y: number) {}
        len() { return Math.hypot(this.x, this.y); }
    }
    export const origin = new Point(0, 0);
    export function unit() { return new Point(1, 0); }
    export const { sx, sy } = { sx: 3, sy: 4 };
    export let [first, , third] = [1, 2, 3];
    export let unset: number;
    export function setUnset(v: number) { unset = v; }
    export const dist = new Point(sx, sy).len();
}
console.log("class", Shapes.origin instanceof Shapes.Point, Shapes.unit().x, Shapes.dist);
console.log("patterns", Shapes.sx, Shapes.sy, Shapes.first, Shapes.third);
console.log("no initializer", "unset" in Shapes, Shapes.unset);
Shapes.setUnset(5);
console.log("set", Shapes.unset);
console.log("keys", Object.keys(Shapes).join(","));

// A nested namespace or enum exported from one lives on the outer object; a
// non-exported one is local to the body.
namespace Outer {
    export namespace Inner {
        export const a = 1;
        export function twice() { return a * 2; }
    }
    namespace Hidden {
        export const b = Inner.a + 100;
    }
    export enum Mode { Off, On }
    export const fromHidden = Hidden.b;
    export const onValue = Mode.On;
}
console.log("nested", Outer.Inner.a, Outer.Inner.twice(), Outer.fromHidden, "Hidden" in Outer);
console.log("enum in namespace", Outer.Mode.On, Outer.Mode[0], Outer.onValue);

// `namespace A.B.C` is A holding an exported B holding an exported C.
namespace Deep.Er.Est {
    export const z = 9;
}
console.log("dotted", Deep.Er.Est.z, Object.keys(Deep).join(","), Object.keys(Deep.Er).join(","));

// The body is an ordinary function, called with the namespace object as its
// one argument.
namespace Body {
    export const argCount = arguments.length;
    export const argIsSelf = arguments[0] === Body;
}
console.log("body", Body.argCount, Body.argIsSelf);

// A namespace of types alone emits nothing.
namespace OnlyTypes {
    export type Id = number;
    export interface Named { name: string }
}
console.log("type-only", typeof (globalThis as any).OnlyTypes);

// Shadowing: a parameter or a local of the same name inside the body is not
// the export.
namespace Shadow {
    export let n = 1;
    export function param(n: number) { return n + 1; }
    export function local() { const n = 50; return n; }
    export function outer() { return n; }
}
Shadow.n = 7;
console.log("shadow", Shadow.param(1), Shadow.local(), Shadow.outer());
