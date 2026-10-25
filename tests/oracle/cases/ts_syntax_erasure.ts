// TypeScript syntax that erases to the JavaScript under it: annotations,
// generics, assertions, overloads, abstract members, type predicates.

// Generic arrows, including the `<T,>` spelling, and generic calls.
const id = <T,>(x: T): T => x;
const pair = <A, B>(a: A, b: B): [A, B] => [a, b];
const asyncId = async <T>(x: T): Promise<T> => x;
console.log("generic arrows", id(3), pair("a", 1).join(":"), typeof asyncId);
console.log("generic calls", id<number>(4), id<Array<Map<string, Set<number>>>>([]).length);
const made = new Map<string, number>([["k", 1]]);
console.log("generic new", made.get("k"));

// Assertions: `as`, `satisfies`, `!`, `<T>x`, `as const`.
const cfg = { mode: "fast" } satisfies { mode: string };
const viaAs = (cfg as { mode: string }).mode;
const viaPrefix = (<any>cfg).mode;
const list = [1, 2, 3] as const;
const maybe: { a?: { b: number } } = { a: { b: 5 } };
console.log("assertions", cfg.mode, viaAs, viaPrefix, list.length, maybe.a!.b);
let definite!: number;
definite = 2;
console.log("definite", definite);

// Comparisons that are not type arguments: TypeScript reads `a < b, c > (d)`
// as a generic call (see ts_js_relational_not_generic.js for the .js
// reading), but nothing closes these.
const lo = 1, hi = 2;
console.log("relational", lo < hi, hi > lo, lo < hi === true);
const cond = true, w = 4;
console.log("conditional", cond ? (w) : 0, (false ? (w) : (q: number) => q * 3)(2));

// The `this` parameter is not a parameter.
function withThis(this: { k: number }, add: number): number { return this.k + add; }
console.log("this param", withThis.length, withThis.call({ k: 1 }, 2));

// Overload signatures are not functions; the implementation is.
function over(x: string): string;
function over(x: number): number;
function over(x: any): any { return typeof x; }
console.log("overloads", over(1), over("s"), over.length);

class Overloaded {
    m(x: string): string;
    m(x: number): number;
    m(x: any): any { return x; }
}
console.log("method overloads", new Overloaded().m(7));

// Abstract classes and members.
abstract class Animal {
    abstract readonly sound: string;
    abstract speak(): string;
    hello(): string { return "hi " + this.speak(); }
}
class Dog extends Animal {
    readonly sound = "woof";
    speak() { return this.sound; }
}
console.log("abstract", new Dog().hello(), Object.getOwnPropertyNames(Animal.prototype).join(","));

// Type predicates and assertion signatures.
function isStr(v: unknown): v is string { return typeof v === "string"; }
function assertNum(v: unknown): asserts v is number {
    if (typeof v !== "number") throw new Error("not a number");
}
assertNum(1);
console.log("predicates", isStr("s"), isStr(1));
try { assertNum("x"); } catch (e: unknown) { console.log("asserts threw", (e as Error).message); }

// Tuples, optional and rest elements, labelled.
const tuple: [first: number, second?: string, ...rest: boolean[]] = [1];
type Fn = (a: number) => void;
const fn: Fn = (a) => { console.log("fn type", a); };
fn(tuple.length);

// Optional parameters, index signatures, class member modifiers.
function opt(a: number, b?: number): string { return a + ":" + b; }
class Members {
    [key: string]: unknown;
    private secret = 1;
    protected shared = 2;
    public open = 3;
    static readonly limit: number = 9;
    declare ambient: string;
    optional?: number;
    get total() { return this.secret + this.shared + this.open; }
}
const mem = new Members();
console.log("members", opt(1), mem.total, Members.limit, Object.keys(mem).join(","));

// Interfaces and type aliases are erased wherever they appear.
interface Point { x: number; y: number }
type Pt = Point & { tag?: string };
const pt: Pt = { x: 1, y: 2 };
console.log("types erased", pt.x + pt.y);
