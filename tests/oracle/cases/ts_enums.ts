// TypeScript enums as node's TypeScript transform emits them. A numeric
// member gets a reverse mapping from its value back to its name; a string
// member gets none. A member's initializer is folded when it is a constant
// expression; otherwise it runs where the member is defined, a bare member
// name in it means that member of the enum, and the member gets the reverse
// mapping too, keyed by whatever the initializer evaluated to.

enum Basic { A, B, C = 10, D }
console.log("basic", Basic.A, Basic.B, Basic.C, Basic.D, Basic[11], Basic[1]);
console.log(JSON.stringify(Basic));

enum Strings { Up = "UP", Down = "DOWN", Joined = Up + "-" + Down }
console.log("strings", Strings.Up, Strings.Joined, JSON.stringify(Strings));

function two() { return 2; }
function word() { return "w"; }
enum Computed { A = 1, B = A * two(), C = "abc".length, D = Computed.A + 100, E = word() }
console.log("computed", JSON.stringify(Computed));
console.log("reverse of computed", Computed[2], Computed[3], Computed[101], Computed["w"]);

// After a string or a computed member, a member with no initializer has no
// value to count on from; node's transform gives it undefined.
enum AfterString { S = "s", T }
console.log("after string", AfterString.T, AfterString["undefined"]);

// Inside the enum body a member name means the member, shadowing an outer
// binding of the same name.
const x = 1000;
enum Shadowing { x = 5, y = x * 2, z = Shadowing.x + x }
console.log("shadowing", Shadowing.y, Shadowing.z, x);

// Constant folding follows the Number semantics of each operator: ToInt32 on
// the bitwise ones, IEEE remainder, `**` with ECMAScript's NaN cases.
enum Folded {
    Frac = 0.5,
    Next,
    Neg = -1,
    Inf = 1 / 0,
    NegZero = -0,
    Big = 1e21,
    Sum = 0.1 + 0.2,
    Concat = "s" + 0.1,
    Exp = 2 ** -1,
    NanMod = 5 % 0,
    Not = ~2.9,
    Shl = 1 << 31,
    UShr = -1 >>> 28,
    Shr = -16 >> 2,
    Or = 1.9 | 0,
    NanOr = (0 / 0) | 0,
    InfAnd = (1 / 0) & 7,
    Wrap = 4294967297 | 0,
    PowNan = 1 ** (1 / 0),
}
console.log("folded", Folded.Frac, Folded.Next, Folded.Neg, Folded.Inf, Object.is(Folded.NegZero, -0));
console.log("folded", Folded.Big, Folded.Sum, Folded.Concat, Folded.Exp, Folded.NanMod, Folded.Not);
console.log("folded", Folded.Shl, Folded.UShr, Folded.Shr, Folded.Or, Folded.NanOr, Folded.InfAnd);
console.log("folded", Folded.Wrap, Folded.PowNan);
console.log("folded keys", Object.keys(Folded).join(","));

// String-named members.
enum Quoted { "a b" = 1, "c" = 2, d = Quoted["a b"] + Quoted.c }
console.log("quoted", Quoted["a b"], Quoted.c, Quoted.d, Quoted[3]);

// A `const enum` is emitted as an ordinary enum by node's transform, and a
// `declare enum` is ambient: nothing is emitted for it.
const enum Flags { None = 0, Read = 1 << 0, Write = 1 << 1, Both = Read | Write }
console.log("const enum", Flags.Both, Flags.Read | Flags.Write);
declare enum Ambient { A }
console.log("declare enum", typeof (globalThis as any).Ambient);

// An enum declared inside a function is local to it.
function makeEnum() {
    enum Local { P = 7, Q }
    return Local;
}
console.log("local", makeEnum().Q, makeEnum()[8]);

// Enum values used in ordinary code.
function describe(c: Basic): string {
    switch (c) {
        case Basic.A: return "is A";
        case Basic.C: return "is C";
        default: return "other " + Basic[c];
    }
}
console.log(describe(Basic.A), describe(Basic.C), describe(Basic.D));
