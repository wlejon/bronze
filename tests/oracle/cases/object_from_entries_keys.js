// Object.fromEntries takes any property key: numbers, booleans, null and
// undefined become their strings, as `o[k] = v` does. A number key threw
// "property key must be a string or a symbol" (broworkshop's synth built its
// EQ spec from `bands.map((label, i) => [i, ...])`).
const byIndex = Object.fromEntries(['low', 'mid', 'high'].map((label, i) => [i, label]));
console.log(JSON.stringify(byIndex), byIndex[1], Object.keys(byIndex).join(','));
const mixed = Object.fromEntries([[1.5, 'a'], [-0, 'z'], [true, 'b'], [null, 'c'], [undefined, 'd'], ['s', 'e']]);
console.log(JSON.stringify(mixed));
const sym = Symbol('k');
const withSym = Object.fromEntries([[sym, 1], ['__proto__', 2]]);
console.log(withSym[sym], Object.getPrototypeOf(withSym) === Object.prototype, Object.keys(withSym).join(','));
