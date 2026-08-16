// ECMA-262 7.2.14 IsLooselyEqual steps 11 and 12: an object against a
// primitive is ToPrimitive'd with NO hint and the comparison RESTARTS.
//
// The restart is the subject. Each step converts exactly ONE operand and asks
// the whole question again, so `[] == false` takes three passes — the boolean
// becomes 0 (step 10), the array becomes "" (step 12), and the string becomes 0
// (step 7) — and the order of those steps is what decides the answer. Running
// them in any other order makes `null == 0` true.
//
// Hint DEFAULT, not string: `valueOf` is asked first, which is why
// `{valueOf: () => 1} == 1` is true and `== '1'` is true for a different reason
// than `String(o) === '1'` would be.
//
// `cases/loose_equality` pins the primitive table this sits on top of; what is
// here is only the half that has to run user code.

// 20.1.3.7's `valueOf` answers with the array itself, so step 3.d carries on to
// `toString`, which is 23.1.3.30's `join`.
console.log([1] == 1, [] == 0, [1, 2] == '1,2', [[]] == 0);
console.log([null] == 0, [undefined] == 0, ({}) == '[object Object]');

// Three passes: boolean to number, object to string, string to number.
console.log([] == false, [0] == false, [1] == true, [2] == true);

// A primitive WRAPPER is the object whose conversion makes `==` and `===`
// disagree, which is the whole observable difference between a wrapper and what
// it wraps.
console.log(new String('ab') == 'ab', new String('ab') === 'ab');
console.log(new Number(3) == 3, new Boolean(true) == 1, new Boolean(false) == 0);

// The wrapper's OVERRIDDEN `valueOf` is what answers, because the real
// algorithm runs rather than a read of the internal slot.
const boxed = new Number(3);
boxed.valueOf = function () { return 9; };
console.log(boxed == 3, boxed == 9);

// Hint default asks `valueOf` first, so an object defining both compares by the
// number and prints as the string.
const both = { valueOf() { return 1; }, toString() { return '2'; } };
console.log(both == 1, both == '1', both == '2', String(both));

// `Symbol.toPrimitive` (7.1.1 step 2) wins, and `==` passes "default".
const hints = [];
const hinted = {
    [Symbol.toPrimitive](hint) { hints.push(hint); return 7; },
};
console.log(hinted == 7, 7 == hinted, hinted == '7', hints.join(','));

// Steps 11 and 12 name Symbol among the types whose object counterpart is
// converted, so a hook answering with a symbol really can be equal to one.
const tag = Symbol('tag');
const wrapsSymbol = { [Symbol.toPrimitive]() { return tag; } };
console.log(wrapsSymbol == tag, tag == wrapsSymbol, wrapsSymbol == Symbol('tag'));
console.log(tag == 'Symbol(tag)', tag == tag);

// Step 1 answers for two objects by IDENTITY, before any conversion — so
// neither side's method runs at all.
const calls = [];
const counted = { valueOf() { calls.push('v'); return 1; } };
const otherCounted = { valueOf() { calls.push('w'); return 1; } };
console.log(counted == counted, counted == otherCounted, calls.length);

// Steps 2 and 3 answer before any conversion, which is what keeps a nullish
// operand out of every coercion below it.
console.log(null == undefined, null == 0, undefined == 0, null == false);
const nullish = { valueOf() { calls.push('n'); return 0; } };
console.log(nullish == null, nullish == undefined, calls.length);

// The whole thing negated once, so `!=` is pinned to be the same algorithm.
console.log([1] != 1, [] != 0, null != undefined);

// 7.1.1.1 step 4: neither half answered a primitive. Thrown and catchable.
const noPrimitive = { valueOf() { return {}; }, toString() { return {}; } };
try {
    noPrimitive == 1;
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}

// A throw from inside the conversion is that exception, not a comparison
// result, and the operand that was not converted is never asked.
const seen = [];
const thrower = { valueOf() { seen.push('t'); throw new Error('eq'); } };
try { thrower == 1; } catch (e) { seen.push(e.message); }
try { 1 == thrower; } catch (e) { seen.push(e.message); }
console.log(seen.join(','));
