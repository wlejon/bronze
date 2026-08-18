// Element access on receivers inference proves are Float64Array /
// Float32Array views. The native ops (elem.get.typed / elem.set.typed) skip
// the receiver guard ladder but keep the language's own index rules, and the
// suite runs every case in both inference modes — so every line here is a
// pin that the proven path and the uniform dynamic path agree byte for byte:
// out-of-bounds and non-integral indices, the NaN-through-a-sink identity,
// silent invalid stores, compound and update forms, f32 narrowing, and the
// shadowed-constructor stand-down.

const a = new Float64Array(4);
a[0] = 1.5; a[1] = 2.5; a[2] = 3.5; a[3] = 4.5;

// In-bounds reads through arithmetic sinks.
console.log(a[0] + a[1], a[2] * 2, a[3] - a[0]);

// Invalid or out-of-bounds READ is `undefined`; through a coercing sink that
// is ToNumber(undefined) = NaN.
console.log(a[4] + 0, a[-1] + 0, a[0.5] + 0, a[NaN] + 0);

// The same reads where the raw value is observable must stay `undefined`.
console.log(a[4], a[-1], typeof a[7]);

// `-0` is a canonical index for element 0.
console.log(a[-0] + 0);

// Invalid or out-of-bounds STORE is a silent no-op; nothing grows.
a[4] = 99; a[-1] = 99; a[1.5] = 99; a[NaN] = 99;
console.log(a.length, a[0], a[1], a[2], a[3]);

// The same discard through indices inference PROVES are numbers but which
// are invalid at run time — the pair of paths that once disagreed.
const nanIdx = 0 / 0;
const negIdx = -2.5;
a[nanIdx] = 123; a[negIdx] = 123;
console.log(a.length, a[0], a[1]);

// Compound assignment on elements.
a[0] += 2; a[1] -= 0.5; a[2] *= 2; a[3] /= 3;
console.log(a[0], a[1], a[2], a[3]);

// Updates on elements.
a[0]++; ++a[1]; a[2]--; --a[3];
console.log(a[0], a[1], a[2], a[3]);

// Compound on an out-of-bounds element: the read half is NaN, the store half
// is the same silent no-op a plain store is.
a[9] += 1;
console.log(a.length, a[9]);

// Exponent and modulo compounds.
a[0] **= 2; a[1] %= 2;
console.log(a[0], a[1]);

// Bitwise compounds go through ToInt32 and store the result as the element's
// float kind.
a[0] &= 6; a[1] <<= 3;
console.log(a[0], a[1]);

// Float32 narrows on store: round-to-nearest-even at 24 bits, sign of zero
// kept.
const f = new Float32Array(3);
f[0] = 0.1; f[1] = 16777217; f[2] = -0;
console.log(f[0], f[1], Object.is(f[2], -0), 1 / f[2]);

// An f32 read promotes exactly, so arithmetic sees the narrowed value.
console.log(f[0] + f[0]);

// A binding fed by a typed read but observed raw must stay `undefined`,
// never a NaN that leaked out of the coercing representation.
const oob = a[100];
console.log(oob === undefined, oob + 0);

// Huge and non-integral indices, raw and through a sink.
console.log(a[4294967296], a[2.000000001], a[Infinity]);
console.log(a[4294967296] + 0, a[Infinity] + 0);

// A store VALUE that needs ToNumber still coerces before storing.
a[0] = "6.25"; a[1] = true; a[2] = null;
console.log(a[0], a[1], a[2]);

// A shadowed Float64Array is not the builtin: the proof stands down and the
// user constructor's object behaves like any object.
{
  const Float64Array = function (n) { this.len = n; this[0] = 7; };
  const s = new Float64Array(2);
  s[0] += 1;
  console.log(s[0], s.len);
}
