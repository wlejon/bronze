// Guards of one site that miss to one far block reach it through each other
// (brass codegen/branch_relay.hpp: a 2-byte jcc onto a nearby branch to the
// same target whose condition its own implies). A function long enough that
// its misses sit far past a rel8, every receiver kind through
// each guard (object, plain or not, each shape, a number, a string, null),
// and exceptions thrown through frames that hold values across calls, so a
// callee-saved register restored wrong by the unwinder shows in the sums.

class A { constructor(v) { this.a = v; this.b = v + 1; this.c = v + 2; } }
class B extends A { constructor(v) { super(v); this.d = v * 2; } }

function big(o, k) {
    // Many reads of `o` and `k`: each a static or cached guard chain whose
    // miss block is laid out after the hot code.
    let s = 0;
    s += o.a; s += o.b; s += o.c;
    if (k > 3) s += o.a * k;
    if (k > 5) s += o.b * k;
    if (k > 7) s += o.c * k;
    s += k.length === undefined ? 0 : k.length;
    s += o.d === undefined ? 1 : o.d;
    s += o.a + o.b + o.c;
    s += (o.a | 0) ^ (o.b | 0);
    s += (o.c & 7) + (o.a >>> 1);
    s += o.b > o.a ? 1 : 0;
    s += o.c >= o.b ? 2 : 0;
    s += o.a !== o.c ? 4 : 0;
    return s;
}

const recvs = [new A(1), new B(2), { a: 3, b: 4, c: 5 }, { c: 1, b: 2, a: 3 }, [1, 2, 3],
    Object.create(null), new A(7.5), new B(-3)];
const keys = [0, 4, 6, 8, 10, 2.5];
let total = 0, nans = 0;
for (let r = 0; r < 300; r++) {
    for (const o of recvs) {
        for (const k of keys) {
            const v = big(o, k);
            if (v === v) total += v; else nans++;
        }
    }
}
console.log('big', total, nans);

// Non-objects through the same reads: each guard's miss runs.
const odd = [1, 'str', true];
let oddOut = [];
for (const o of odd) oddOut.push(String(big(o, 4)) + '/' + String(big({ a: o, b: o, c: o }, 6)));
console.log('odd', oddOut.join(' '));
try { big(null, 1); } catch (e) { console.log('null', e instanceof TypeError); }
try { big(undefined, 1); } catch (e) { console.log('undef', e instanceof TypeError); }

// Values live across calls (callee-saved registers) in frames an exception
// unwinds through, at several depths.
function thrower(n) { if (n % 3 === 0) throw new Error('t' + n); return n * 2; }
function mid(n, o) {
    const x = o.a * 3, y = o.b - 1, z = n + 11;
    const r = thrower(n);
    return x + y + z + r + o.c;
}
function outer(n, o) {
    const p = n * 5, q = o.a + n;
    let got = 0;
    try { got = mid(n, o); } catch (e) { got = -e.message.length - p; }
    return got + p + q + o.b;
}
let sum = 0;
for (let i = 0; i < 2000; i++) sum += outer(i, recvs[i % 3]);
console.log('unwind', sum);
