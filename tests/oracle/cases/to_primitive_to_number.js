// ECMA-262 7.1.4 ToNumber with an OBJECT argument: step 1 is ToPrimitive with
// hint NUMBER, and everything after it is the conversion of the primitive that
// produced.
//
// This is the conversion generated code performs most: every arithmetic
// operator but `+` unboxes both operands through it, and so do the bitwise
// operators (7.1.6 ToInt32 step 1), `Number()`, and every `Math` function. So
// the hint is asked for once, here, and `valueOf` answers before `toString` for
// all of them — which is why `{valueOf: () => 1, toString: () => '2'} * 1` is 1
// and `String()` of the same object is "2".
//
// The half that is not about arithmetic: a conversion that runs user code can
// THROW, from `valueOf` itself or from 7.1.1.1 step 4, and generated code has to
// have an unwind path after an unbox for a `catch` to see it. Every `try` below
// is pinning that path rather than the arithmetic.

const three = { valueOf() { return 3; } };
console.log(three * 2, three - 1, three / 6, three % 2, three ** 2);
console.log(+three, -three, Number(three), Math.abs(three), Math.max(three, 2));

// The primitive `valueOf` produced is then ToNumber'd in the ordinary way, so a
// numeric STRING is parsed and anything else is NaN.
const stringy = { valueOf() { return '4'; } };
console.log(stringy * 2, +stringy, Number(stringy));
const wordy = { valueOf() { return 'nope'; } };
console.log(wordy * 2, +wordy, Number.isNaN(+wordy));

// `valueOf` answering with an object falls through to `toString`, whose result
// is what gets parsed.
const fallback = { valueOf() { return {}; }, toString() { return '10'; } };
console.log(fallback * 2, +fallback, fallback - 4);

// Hint number and hint string on the same object, side by side.
const both = { valueOf() { return 1; }, toString() { return '2'; } };
console.log(both * 1, String(both), `${both}`, both + 1, both + '');

// The other primitives `valueOf` can answer with.
console.log(+{ valueOf() { return true; } }, +{ valueOf() { return null; } });
console.log(Number.isNaN(+{ valueOf() { return undefined; } }));

// 7.1.6 ToInt32 step 1 is this same ToNumber, so the bitwise family converts
// objects too — and truncates afterwards.
const fractional = { valueOf() { return 5.9; } };
console.log(fractional | 0, fractional & 3, fractional << 1, ~fractional);
console.log(({ valueOf() { return '12'; } }) & 10, ({ valueOf() { return -1; } }) >>> 28);

// A primitive WRAPPER runs the real algorithm, so an overridden `valueOf` is
// what answers rather than the internal slot.
const boxed = new Number(2);
console.log(boxed * 3, +boxed);
boxed.valueOf = function () { return 50; };
console.log(boxed * 3, +boxed, Number(boxed));

// `Symbol.toPrimitive` wins, and every arithmetic position passes "number".
const hints = [];
const hinted = { [Symbol.toPrimitive](hint) { hints.push(hint); return 6; } };
console.log(hinted * 2, hinted - 1, +hinted, Number(hinted), Math.sqrt(hinted));
console.log(hinted | 0, hints.join(','));

// Compound assignment and the update operators are the same conversion.
let acc = { valueOf() { return 8; } };
acc -= 3;
console.log(acc);
let counter = { valueOf() { return 41; } };
counter++;
console.log(counter);

// The unwind path after an unbox: a `valueOf` that throws inside arithmetic is
// catchable, in every operator position.
function raises() { return { valueOf() { throw new Error('boom'); } }; }
try { raises() * 2; } catch (e) { console.log('mul', e.message); }
try { -raises(); } catch (e) { console.log('neg', e.message); }
try { raises() | 0; } catch (e) { console.log('bit', e.message); }
try { Math.floor(raises()); } catch (e) { console.log('math', e.message); }
try { Number(raises()); } catch (e) { console.log('number', e.message); }

// 7.1.1.1 step 4 in an arithmetic position, likewise catchable.
const noPrimitive = { valueOf() { return {}; }, toString() { return {}; } };
try { noPrimitive * 2; } catch (e) { console.log(e instanceof TypeError, e.message); }
try { noPrimitive | 0; } catch (e) { console.log(e instanceof TypeError, e.message); }

// The conversion happens once per operand, in source order.
const order = [];
const a = { valueOf() { order.push('a'); return 2; } };
const b = { valueOf() { order.push('b'); return 3; } };
console.log(a * b, order.join(','));

// A typed array element write is ToNumber on the VALUE (10.4.5.5), so it runs
// the conversion too — and the view survives whatever the conversion allocated.
const view = new Float64Array(3);
view[0] = { valueOf() { return 1.5; } };
view[1] = { toString() { return '2.5'; } };
view[2] = new Number(3.5);
console.log(view[0], view[1], view[2]);
