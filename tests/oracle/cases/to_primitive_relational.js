// ECMA-262 13.10 and 13.10.1 IsLessThan with an OBJECT operand: step 1 is
// ToPrimitive with hint NUMBER, so `valueOf` is asked before `toString` and a
// `Symbol.toPrimitive` before either.
//
// The hint is the half that is easy to get backwards. `o < 1` asks for NUMBER
// where `String(o)` asks for STRING, so an object defining both halves compares
// by what `valueOf` says and prints what `toString` says — and the two really do
// disagree.
//
// Step 3 is the other half: after the conversion, two Strings are compared by
// CODE UNIT with nothing converted, so `{toString:()=>'10'} < '9'` is true where
// `{toString:()=>'10'} < 9` is false. The digits are read as digits only in the
// else-branch, step 4.
//
// The ORDER of the two conversions is 13.10.1's LeftFirst flag, and it is
// observable now that each one can run a program's method. `a > b` asks
// IsLessThan(b, a) and passes LeftFirst false, which is what keeps `a`'s
// `valueOf` running first for all four operators — the operand order follows the
// SOURCE, not the argument list.

const three = { valueOf() { return 3; } };
console.log(three < 5, three > 5, three <= 3, three >= 4);

// Hint number, so `valueOf` answers and `toString` is never asked.
const both = { valueOf() { return 2; }, toString() { return 'zzz'; } };
console.log(both < 3, both < 'a', String(both));

// Only `toString`: 20.1.3.6's `valueOf` answers with the object itself, which
// is not a primitive, so step 3 carries on to the second method.
const bee = { toString() { return 'b'; } };
console.log(bee < 'c', 'a' < bee, bee < 'a');

// Step 3 against step 4: the same object is a string comparison on one line and
// a numeric one on the next.
const numish = { toString() { return '10'; } };
console.log(numish < '9', numish < 9, numish >= 9);

// A `valueOf` that answers with an OBJECT falls through to `toString`
// (7.1.1.1 step 3.d), and the string it produced is then step 3's operand.
const fallback = { valueOf() { return {}; }, toString() { return 'M'; } };
console.log(fallback < 'N', fallback > 'L', fallback <= 'M');

// `Symbol.toPrimitive` (7.1.1 step 2) wins outright and receives "number".
const hints = [];
const hinted = {
    [Symbol.toPrimitive](hint) { hints.push(hint); return 4; },
};
console.log(hinted < 5, hinted > 5, hinted <= 4, hinted >= 9, hints.join(','));

// The conversion order, recorded. All four operators convert the SOURCE's left
// operand first, including the two that swap the arguments of IsLessThan.
const order = [];
function probe(name, n) {
    return { valueOf() { order.push(name); return n; } };
}
const left = probe('L', 1);
const right = probe('R', 2);
function orderOf(run) {
    order.length = 0;
    run();
    return order.join(',');
}
console.log(orderOf(() => left < right), orderOf(() => left > right));
console.log(orderOf(() => left <= right), orderOf(() => left >= right));

// A throw from the FIRST conversion stops the comparison, so the second operand
// is never asked. `>` swaps the arguments and must still stop the same way.
const seen = [];
const thrower = { valueOf() { seen.push('L'); throw new Error('stop'); } };
const other = { valueOf() { seen.push('R'); return 1; } };
try { thrower < other; } catch (e) { seen.push(e.message); }
try { thrower > other; } catch (e) { seen.push(e.message); }
console.log(seen.join(','));

// Step 4.c: a NaN on either side is *undefined*, which 13.10 folds to false for
// all four operators — a negation would have called two of them true.
const notANumber = { valueOf() { return NaN; } };
console.log(notANumber < 1, notANumber > 1, notANumber <= 1, notANumber >= 1);
const nothing = { valueOf() { return undefined; } };
console.log(nothing < 1, nothing >= 1);

// A primitive WRAPPER goes through the real algorithm rather than through its
// internal slot, so an OVERRIDDEN `valueOf` is the one that answers.
console.log(new String('a') < 'b', new Number(2) < 3);
const boxed = new Number(5);
boxed.valueOf = function () { return 100; };
console.log(boxed < 50, boxed > 50);

// Both halves answering with an object is 7.1.1.1 step 4's TypeError, thrown
// and catchable.
const noPrimitive = { valueOf() { return {}; }, toString() { return {}; } };
try {
    noPrimitive < 1;
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}

// A Symbol reaching step 4 is 6.1.5.1's TypeError, and it is catchable too —
// step 3 only takes it out of the way when BOTH operands are Strings.
const symbolic = { [Symbol.toPrimitive]() { return Symbol('s'); } };
try {
    symbolic < 1;
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}

// Two numbers still convert to themselves, which is the shape every typed
// comparison arrives in.
console.log(1 < 2, 2 <= 2, 'ab' < 'b', 'b' <= 'ab');
