// A data property of a module-scope object literal that can only ever hold the
// value the literal wrote into it.
//
// `const X = { q: 'k', get p() { return this.q; } };` where nothing in the
// program writes `q` or `p`, and nothing ever holds `X` itself, makes `X.q` —
// and `X.p`, which is a call whose body is that read — the string `'k'` at
// every read. A primitive is a VALUE: it has no identity apart from itself, so
// "the same primitive" and "the same read" are the same claim. This case pins
// what those reads answer, at every spelling, hot and cold, for a string, a
// number, a boolean, `null` and `undefined`, and for an initializer that is a
// module-scope `const` NAME rather than a literal written in place.
//
// It also pins the object itself: an accessor property and a data property,
// their descriptors, their enumeration order, `in`, and what `JSON.stringify`
// makes of them. That half is spelled on a SECOND literal, `Mirror`, whose
// shape is `Fold`'s shape with the names changed — and that is not a
// convenience. Every one of `getOwnPropertyDescriptor`, `keys`, `in` and
// `stringify` takes the object as an ARGUMENT, and a literal handed to code as
// a value is a literal nothing may claim anything about; asking those questions
// OF `Fold` would be a program in which `Fold` is not certified, so the case
// would pin the reflection and stop pinning the fold. Two literals pin both:
// `Mirror` shows what the properties are, `Fold` shows what reading them
// answers, and `Fold.label` reads the backing slot through `this` — a receiver
// no fold applies to — so a fold that disagreed with the object it folded
// would print two different strings on one line.

// --- the certified shape --------------------------------------------------
// A module-scope `const` bound to a string literal, read as a property
// initializer below. 13.2.5.5 evaluates the initializer once, when the literal
// is evaluated, so the property holds the string and not the name.
const KEY = 'srgb-linear';

const Fold = {
  _working: KEY,
  count: 3,
  enabled: true,
  missing: null,
  absent: undefined,
  get working() { return this._working; },
  // Never entered. 10.1.9.1 calls a setter only from a [[Set]] of ITS key, and
  // nothing in this program writes `working`.
  set working(v) { this._working = v; },
  // `this` here is `Fold`, and this read is spelled on `this`, so it is the
  // object's own answer and not any claim about it.
  label(tag) { return tag + '=' + this._working; }
};

// 10.1.8.1 OrdinaryGet: `working` is an own accessor, so [[Get]] calls the
// getter with the receiver as `this`, and the body is one read of `_working`.
console.log('accessor      ' + Fold.working);
// 13.3.2 and 13.3.3 both ToPropertyKey to the string "working", so the two
// spellings name one property.
console.log('bracket       ' + Fold['working']);
// The backing data property, read directly.
console.log('backing       ' + Fold._working);
// Through `this`, which nothing forwards or folds.
console.log('through this  ' + Fold.label('cs'));
// 7.2.16: two Strings are strictly equal when their code units are.
console.log('identity      ' + (Fold.working === KEY) + ' ' + (Fold.working === Fold._working));

// The same read a thousand times over. An answer that drifts is a constant
// that outlived its proof.
let hot = 0;
for (let i = 0; i < 1000; i++) {
  if (Fold.working === 'srgb-linear') hot++;
}
console.log('hot           ' + hot);

// The shape the whole thing exists for: a parameter default, evaluated per
// call, only when the argument is `undefined` (10.2.11 / 8.6.3).
function shade(c, cs = Fold.working) { return c + '/' + cs; }
console.log('default       ' + shade('red') + ' ' + shade('red', 'display-p3'));
console.log('default undef ' + shade('red', undefined));

// --- the other four primitive kinds ---------------------------------------
// A number is a number and not a string: 7.1.4 ToNumber is never reached by
// `*`'s operands here, and 13.15.3's `+` on two Numbers adds.
console.log('number        ' + Fold.count + ' ' + (Fold.count * 2) + ' ' + (Fold.count + Fold.count));
console.log('number type   ' + typeof Fold.count);
// 13.5.3 typeof null is "object", and that is the one it must stay.
console.log('boolean       ' + Fold.enabled + ' ' + typeof Fold.enabled + ' ' + (Fold.enabled ? 'y' : 'n'));
console.log('null          ' + Fold.missing + ' ' + typeof Fold.missing + ' ' + (Fold.missing === null));
// An own property whose value is `undefined` is still an own property; only
// its VALUE is `undefined`.
console.log('undefined     ' + Fold.absent + ' ' + typeof Fold.absent + ' ' + (Fold.absent === undefined));

let sum = 0;
for (let i = 0; i < 1000; i++) {
  sum += Fold.count;
  if (Fold.enabled) sum++;
}
console.log('hot number    ' + sum);

// --- what the object actually is ------------------------------------------
const Mirror = {
  _base: KEY,
  size: 3,
  get base() { return this._base; },
  set base(v) { this._base = v; }
};

// 13.2.5.5 defines a data property through CreateDataPropertyOrThrow, and
// 7.3.5's default descriptor is writable, enumerable and configurable.
const dData = Object.getOwnPropertyDescriptor(Mirror, '_base');
console.log('desc data     ' + dData.value + ' w=' + dData.writable + ' e=' + dData.enumerable +
            ' c=' + dData.configurable);
// 6.2.6.4 FromPropertyDescriptor gives an ACCESSOR descriptor object `get`,
// `set`, `enumerable` and `configurable` — and neither `value` nor `writable`.
const dAcc = Object.getOwnPropertyDescriptor(Mirror, 'base');
console.log('desc accessor get=' + typeof dAcc.get + ' set=' + typeof dAcc.set +
            ' e=' + dAcc.enumerable + ' c=' + dAcc.configurable);
console.log('desc halves   value=' + ('value' in dAcc) + ' writable=' + ('writable' in dAcc));
// The getter really is the function that answers, so calling it with an
// explicit receiver answers that receiver's `_base`.
console.log('desc get call ' + dAcc.get.call(Mirror));

// 10.1.11.1 OrdinaryOwnPropertyKeys: no key here is an array index, so the
// order is creation order — and an accessor takes its place in that order at
// the point its first half was defined.
console.log('keys          ' + Object.keys(Mirror).join(','));
// 13.10.1 HasProperty, which an accessor answers just as a data property does.
console.log('in            ' + ('base' in Mirror) + ' ' + ('_base' in Mirror) + ' ' +
            ('nope' in Mirror));
// 25.5.2 SerializeJSONObject walks EnumerableOwnProperties in that same order
// and READS each one, so the getter runs and its value is what lands.
console.log('json          ' + JSON.stringify(Mirror));

// The same shape as `Fold`, read the same way, so the two must agree.
console.log('mirror read   ' + Mirror.base + ' ' + Mirror._base + ' ' + Mirror.size);
