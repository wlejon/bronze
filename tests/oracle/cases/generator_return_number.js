// A generator whose `return` operand is a NUMBER, in every form a generator can
// take. The type matters, which is the whole reason this case is separate from
// `generator_return_statement.js` — every `return` in that one hands back a
// string, and a string is why it could not catch this.
//
// ECMA-262 27.5.1.2: calling a generator function runs NONE of the body. It
// builds a generator object and returns that. So what the body returns is the
// `value` of the final result (27.5.3.2), and it says nothing whatever about
// what `f()` itself evaluates to.
//
// Inference that read the `return` statements to type the call would therefore
// tell every caller to expect a number and hand it an object — and a number is
// the one type that makes that visible, because the others need no coercion to
// go wrong. A generator's signature is the object, always, however plain its
// body looks.
//
// Every expected byte below was derived from the specification before bronze
// was run.

// --- a declaration whose body is nothing but a numeric return --------------
function* plain() {
    return 7;
}
const p = plain();
console.log(p.next());
// 27.5.3.2: completed stays completed, and the operand is not handed out twice.
console.log(p.next());

// --- a numeric return after a numeric yield --------------------------------
function* afterYield() {
    yield 1;
    return 2;
}
const a = afterYield();
console.log(a.next());
console.log(a.next());
// 13.2.4.1: spread collects what is yielded and discards the completion value,
// so the 2 does not appear.
console.log([...afterYield()]);

// --- a generator expression, with the operand computed from a parameter -----
const doubler = function* (x) {
    return x * 2;
};
console.log(doubler(21).next());

// --- a class generator method, whose operand reads `this` ------------------
class Box {
    constructor(n) {
        this.n = n;
    }
    *half() {
        return this.n / 2;
    }
}
console.log(new Box(10).half().next());

// --- a generator built inside a closure and called through the returned
// binding, which is the shape that keeps a signature from being direct-called
function makeCounter(start) {
    function* one() {
        return start + 1;
    }
    return one;
}
console.log(makeCounter(41)().next());

// --- gen.return(v) with a numeric argument (27.5.3.3) ----------------------
function* loop() {
    let i = 0;
    while (true) {
        yield i;
        i++;
    }
}
const l = loop();
console.log(l.next().value, l.next().value);
console.log(l.return(99));
console.log(l.next());

// --- the value survives arithmetic at the call site ------------------------
console.log(plain().next().value + 1);
