// What a closure CAPTURES is decided by where a mention sits: a name bound by
// some block inside the closure is still free when the mention is outside that
// block. The capture walk once subtracted every declaration anywhere in the
// body from every mention in it, so each read below resolved against the
// global object and threw.

let x = 1;
function blockShadow() { { let x = 2; } return x; }
console.log("block:", blockShadow());

let y = 1;
function forOfShadow() { for (let y of [5]) {} return y; }
console.log("for-of head:", forOfShadow());

let a = 1;
function patternShadow() { for (const [a] of [[7]]) {} return a; }
console.log("pattern head:", patternShadow());

let k = 1;
function forInShadow() { for (const k in { q: 1 }) {} return k; }
console.log("for-in head:", forInShadow());

let i = 1;
function forShadow() { for (let i = 0; i < 3; i++) {} return i; }
console.log("for head:", forShadow());

let e = 1;
function catchShadow() { try { throw 0; } catch (e) {} return e; }
console.log("catch param:", catchShadow());

let s = 1;
function switchShadow(v) {
    switch (v) { case 0: { let s = 9; return s; } default: return s; }
}
console.log("switch:", switchShadow(0), switchShadow(1));

let p = 1;
function paramShadow() { const inner = (p) => p * 10; return inner(2) + p; }
console.log("nested param:", paramShadow());

// A `var` is the function's wherever it is written, so the read after the
// block sees the block's store.
function varHoist() { { var h = 3; } return h; }
console.log("var hoist:", varHoist());

// The loop's declared head is the loop's binding, and the assigning form
// writes the outer one — inside a function as at the top level.
let last = "none";
function assignHead() { for (last of ["p", "q"]) {} }
assignHead();
console.log("assigning head:", last);

// A named function expression sees itself and nothing else by that name.
let fact = "outer";
function selfName() {
    return (function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); })(4) + " " + typeof fact;
}
console.log("own name:", selfName());

// A class EXPRESSION sees its own name inside its body and nowhere else; the
// outer binding of the same spelling is a different one. Without a record for
// that name, the method read the outer `C` — or, with no outer `C`, threw.
let C = "outer";
function className() {
    const K = class C { static who() { return typeof C; } make() { return new C(); } };
    return K.who() + " " + C + " " + (new K().make() instanceof K) + " " + typeof C;
}
console.log("class expr:", className());
const Anon = class D { static self() { return D === Anon; } };
console.log("class expr self:", Anon.self());

// A class DECLARATION inside a function is that function's binding, reached
// from its own methods through the function's environment.
function classDecl() {
    class E { static make() { return new E(); } }
    return E.make() instanceof E;
}
console.log("class decl:", classDecl());
