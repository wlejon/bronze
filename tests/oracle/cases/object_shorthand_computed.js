// Object literal shorthand and computed keys.
//
// Derived from ECMA-262 13.2.5. `{ x }` is IdentifierReference shorthand: the
// property name is the identifier's text and the value is the result of
// evaluating it as a reference, so it is exactly `{ x: x }`. `{ [e]: v }` is
// a ComputedPropertyName: PropertyDefinitionEvaluation evaluates `e`, applies
// ToPropertyKey to the result (7.1.19 — ToString for anything that is not a
// Symbol), and only THEN evaluates `v`. Property definitions run left to
// right, so a later definition with the same key overwrites an earlier one,
// and the key's evaluation is interleaved with the values' in source order.
//
// Own-key order is docs/0009's: integer-like keys ascending, then the rest in
// insertion order — and a computed key that ToPropertyKey turns into "2" is
// an integer-like key however it was written.

const x = 1;
const y = 'two';
const o = { x, y };
console.log(o);
console.log(o.x);
console.log(o.y);

// Shorthand mixed with ordinary properties, in both orders.
const z = true;
console.log({ x, a: 10, z });
console.log({ a: 10, x });

const k = 'dyn';
const c = { [k]: 10, plain: 20 };
console.log(c);
console.log(c[k]);
console.log(c.dyn);

// ToPropertyKey of a number is its ToString, so this names the property "2",
// which is integer-like and therefore enumerates first.
const n = { b: 1, [2]: 'two', a: 3 };
console.log(Object.keys(n).join(','));
console.log(n[2]);
console.log(n.b + n.a);

// A key computed from an expression, and one computed from a call.
const prefix = 'item';
function keyFor(i) { return prefix + i; }
const built = { [prefix + '0']: 'zero', [keyFor(1)]: 'one' };
console.log(built.item0);
console.log(built.item1);
console.log(Object.keys(built).join(','));

// Evaluation order: key then value, left to right across the literal.
let log = '';
function t(tag, value) { log = log + tag; return value; }
const ord = { [t('k1', 'a')]: t('v1', 1), [t('k2', 'b')]: t('v2', 2) };
console.log(log);
console.log(ord.a + ord.b);

// A later definition wins, whichever spelling produced the key.
const dup = { p: 1, ['p']: 2 };
console.log(dup.p);
const dup2 = { ['q']: 1, q: 2 };
console.log(dup2.q);

// Shorthand picks up the binding that is in scope where the literal is
// written, including a captured one.
function make(v) {
  const w = v * 2;
  return { v, w };
}
console.log(make(3));

// An empty literal and a literal of only shorthand still print as objects.
console.log({});
const single = 7;
console.log({ single });
