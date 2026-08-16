// ECMA-262 6.1.5.1: ToNumber of a Symbol is a TypeError and ToString of one is
// a TypeError. Both are THROWN and catchable, which is the whole point of the
// type — an accidental coercion is loud, and a program can decide what to do
// about it.
//
// The two escapes are deliberate and are pinned here beside the throws:
// `String(sym)` and `sym.toString()` ask for the text in so many words
// (20.4.3.3, 20.4.3.5), so neither goes through the coercion. A template
// substitution is ToString and DOES, which is the pair most likely to surprise.
//
// Catchable is the fact under test. `sym * 2` unboxes both operands, and an
// unbox is generated code's own numeric coercion — so this case fails as a
// process death rather than as wrong bytes if the unwind path after one is
// missing.

const sym = Symbol('id');

function attempt(label, run) {
    try {
        run();
        console.log(label, 'no throw');
    } catch (e) {
        console.log(label, e instanceof TypeError, e.message);
    }
}

// Every arithmetic operator reaches ToNumber.
attempt('mul', () => sym * 2);
attempt('sub', () => sym - 1);
attempt('div', () => sym / 2);
attempt('mod', () => sym % 2);
attempt('pow', () => sym ** 2);
attempt('unary plus', () => +sym);
attempt('negate', () => -sym);

// 7.1.6 ToInt32 step 1 is that same ToNumber.
attempt('bitor', () => sym | 0);
attempt('bitnot', () => ~sym);
attempt('shift', () => sym << 1);

// The named conversions and the builtins that perform one.
attempt('Number', () => Number(sym));
attempt('Math.abs', () => Math.abs(sym));

// `+` is not one operator: 13.15.3 step 3 runs ToString when either side is a
// String after ToPrimitive and ToNumeric otherwise, and 6.1.5.1 refuses both.
attempt('add number', () => sym + 1);
attempt('add string', () => sym + '');

// 13.10.1 step 4 is ToNumeric, so a relational operator refuses too.
attempt('less than', () => sym < 1);
attempt('greater equal', () => sym >= 1);

// A template substitution is ToString (13.2.8.6) and so is refused, where the
// two spellings that ASK for the text are not.
attempt('template', () => `${sym}`);
console.log(String(sym), sym.toString(), sym.description);

// A typed array element write is ToNumber on the value.
const view = new Int32Array(1);
attempt('typed array write', () => { view[0] = sym; });
console.log(view[0]);

// 7.2.14 converts nothing for a Symbol, so `==` ANSWERS rather than throwing —
// which is what keeps an accidental comparison from becoming an accidental
// exception.
console.log(sym == 1, sym == 'Symbol(id)', sym == sym, sym === sym);

// A symbol is already a property key, so the key position never coerces.
const holder = {};
holder[sym] = 'kept';
console.log(holder[sym], sym in holder, Object.keys(holder).length);

// The unwind path works from inside a loop and a nested call, not just from a
// straight-line `try`.
let caught = 0;
for (let i = 0; i < 3; i++) {
    try {
        if (i * 1 >= 0) sym * i;
    } catch (e) {
        caught++;
    }
}
console.log(caught);

function deep(v) { return v * 2; }
try {
    deep(sym);
} catch (e) {
    console.log('nested', e.message);
}

// And execution carries on afterwards, which a fatal would not have allowed.
console.log('still running', 6 * 7);
