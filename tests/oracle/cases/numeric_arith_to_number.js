// ECMA-262 13.15.3 for `*`, `-`, `/` and `%` over operands nothing typed.
//
// With no BigInt anywhere in a program, ToNumeric IS ToNumber and those four
// operators become 7.1.4 on each side followed by a native f64 operation
// (lower/bigint_reach.h). That lowering is the default for a standalone build,
// so the conversion it performs is the conversion this whole suite runs
// under — and it is 7.1.4 in full, not a fast path with the odd corner
// missing. This case is 7.1.4 asked through the four operators rather than
// through `Number()`.
//
// Every operand arrives through `d`, and that is load-bearing rather than
// decoration: `'3' * '4'` spelled out has two operands inference proves are
// strings, so the expression is settled before the arm is reached and the case
// would pin nothing about it. `d`'s parameter is unproven because the loop
// below calls it with a value of each of seven types, so every `d(...)` here
// is a `dynamic` the operator must convert at run time.
//
// Derived from 7.1.4 (ToNumber), 7.1.4.1 (StringToNumber), 7.1.1 (ToPrimitive
// with hint number) and 6.1.6.1.4 / .5 / .6 (multiply, divide, remainder).

function d(x) { return x; }
const seeds = [1, 'x', null, undefined, true, {}, Symbol('seed')];
for (let i = 0; i < seeds.length; i++) d(seeds[i]);

// Strings that ARE numeric literals: 7.1.4.1 parses them, so nothing here is
// NaN.
console.log(d('3') * d('4'), d('10') / d('4'), d('7') - d('2'), d('7') % d('2'));

// 7.1.4's table for the non-object primitives: undefined is NaN, null is +0,
// true is 1 and false is +0. `null - 1` being -1 rather than NaN is the pair
// most often got wrong, so it leads.
console.log(d(null) - d(1), d(null) * d(5), d(undefined) * d(2), d(true) / d(2),
            d(false) - d(1));

// An array's ToPrimitive is `join` (23.1.3.34), and the STRING that produces is
// what 7.1.4.1 then parses. So `[]` is +0, `[5]` is 5, and `[1, 2]` is NaN
// because "1,2" is not a numeric literal.
console.log(d([]) * d(2), d([5]) - d(1), d([1, 2]) * d(2), d([]) - d([]));

// StringNumericLiteral is not the same grammar as `parseFloat`: it admits a
// hex literal and an exponent, trims StrWhiteSpace from both ends, and makes
// the empty and all-whitespace strings +0.
console.log(d('0x10') * d(1), d('  12  ') / d(4), d('1e3') % d(7), d('') * d(1),
            d('   ') - d(0));

// And it admits nothing else: a trailing unit, a thousands separator and a
// bare word are all NaN, where `Infinity` is a literal the grammar names.
console.log(d('nope') * d(2), d('12px') - d(1), d('1,000') / d(2), d('Infinity') * d(1));

// -0 is a value, not a formatting question, so it is asked through `Object.is`.
// 6.1.6.1.4: the sign of a product is the XOR of its operands' signs, and that
// holds when the magnitude is zero.
console.log(Object.is(d('-0') * d(1), -0), Object.is(d(0) * d(-1), -0),
            Object.is(d(-1) * d(0), -0));

// The same for a quotient, and for the remainder's step 10: a zero remainder
// from a negative dividend is -0.
console.log(Object.is(d(-0) / d(1), -0), Object.is(d(0) / d(-1), -0),
            Object.is(d(-1) % d(1), -0));

// 6.1.6.1.6 truncates the quotient TOWARD ZERO, so the sign of `%` follows the
// dividend and never the divisor. This is where `%` parts company with a
// mathematical modulo.
console.log(d(5) % d(3), d(-5) % d(3), d(5) % d(-3), d(-5) % d(-3));

// `%` over non-integers and over the infinities: a zero divisor and an
// infinite dividend are NaN, and an infinite DIVISOR returns the dividend
// unchanged.
console.log(d(5.5) % d(2), Number.isNaN(d(5) % d(0)), Number.isNaN(d(Infinity) % d(2)),
            d(5) % d(Infinity));

// Division by zero is an infinity rather than a throw, and 0/0 is NaN.
console.log(d(1) / d(0), d(-1) / d(0), d(1) / d(Infinity), Number.isNaN(d(0) / d(0)));

// The infinities under multiplication, including the one indeterminate form.
console.log(Number.isNaN(d(Infinity) * d(0)), d(Infinity) * d(2), d(Infinity) * d(-2),
            Number.isNaN(d(Infinity) - d(Infinity)));

// 13.15.3 evaluates and converts the LEFT operand before the right, so a
// `valueOf` with a side effect reports the order. Two operators, because the
// order is a property of the algorithm and not of `*`.
const mulOrder = [];
const subOrder = [];
function tracer(log, name, value) {
    return { valueOf() { log.push(name); return value; } };
}
console.log(d(tracer(mulOrder, 'left', 6)) * d(tracer(mulOrder, 'right', 7)),
            mulOrder.join(','));
console.log(d(tracer(subOrder, 'left', 7)) - d(tracer(subOrder, 'right', 6)),
            subOrder.join(','));

// ToPrimitive with hint number asks `valueOf` first and falls through to
// `toString` when there is none, or when the one there answers with an object.
const strOnly = { toString() { return '8'; } };
console.log(d(strOnly) * d(2), d(strOnly) - d(3), d(strOnly) / d(4), d(strOnly) % d(3));
const both = { valueOf() { return {}; }, toString() { return '9'; } };
console.log(d(both) * d(2), d(both) % d(4));

// The two TypeErrors reachable from an arithmetic operand — 6.1.5.1 for a
// Symbol, 7.1.1.1 step 4 for an object with no primitive to give — are thrown
// and CATCHABLE from either side. The message is the implementation's, so only
// the type is pinned.
function attempt(label, run) {
    try {
        run();
        console.log(label, 'no throw');
    } catch (e) {
        console.log(label, e instanceof TypeError);
    }
}
attempt('symbol left', () => d(Symbol('s')) * d(2));
attempt('symbol right', () => d(2) % d(Symbol('s')));
const noPrimitive = { valueOf() { return {}; }, toString() { return {}; } };
attempt('no primitive', () => d(noPrimitive) - d(1));

// A throw from the RIGHT operand's conversion still leaves the left one's side
// effect behind, because the left one already ran.
const throwOrder = [];
const bomb = { valueOf() { throwOrder.push('bomb'); throw new Error('x'); } };
try {
    d(tracer(throwOrder, 'left', 6)) * d(bomb);
} catch (e) {
    console.log('threw', e.message, throwOrder.join(','));
}

// And a throw from the LEFT operand's conversion means the right one's never
// runs at all — the half of the ordering an "in source order" claim does not
// by itself pin.
const rightOrder = [];
const leftBomb = { valueOf() { throw new Error('y'); } };
try {
    d(leftBomb) - d(tracer(rightOrder, 'right', 1));
} catch (e) {
    console.log('left threw', e.message, rightOrder.length);
}

// A chain, which is the shape the lowering exists for: every intermediate here
// is a converted operand feeding another operator.
console.log(d('2') * d('3') + d('4') * d('5'), d('10') / d('4') - d('1') % d('3'));
console.log('done', d(6) * d(7));
