// ECMA-262 20.1.3.6 Object.prototype.toString, one line per step that decides
// an answer.
//
// Steps 1 and 2 come BEFORE step 3's ToObject, which is the only reason
// `undefined` and `null` have an answer at all — every other member of 20.1.3
// raises for them.
//
// Steps 4 to 14 are the builtin tag, tried in order: IsArray, then
// [[ParameterMap]], [[Call]], [[ErrorData]], [[BooleanData]], [[NumberData]],
// [[StringData]], step 11's slot for the kind bronze has not got, then
// [[RegExpMatcher]], and "Object" for anything left. (Step 11's slot is
// spelled around rather than out: the suite bans that word in a case, because
// a clock is the one thing a pinned expectation cannot survive.)
//
// The order is what makes an arguments object read "Arguments" rather
// than "Array", since it is an array in bronze (10.2.11 is stood in for by an
// ordinary array carrying a `callee` accessor).
//
// 20.5.6.3 gives `NativeError.prototype` no @@toStringTag, so a TypeError reads
// "[object Error]" exactly as an Error does — the class is not in the answer.
//
// 20.1.3.5 toLocaleString is `Invoke(O, "toString")` with no arguments and no
// locale of any kind, so it reports whatever the receiver's own `toString`
// does.
const ts = Object.prototype.toString;

console.log(ts.call(undefined));
console.log(ts.call(null));
console.log(ts.call([1, 2]));
function args() { return ts.call(arguments); }
console.log(args(1));
console.log(ts.call(args));
console.log(ts.call(new Error('e')));
console.log(ts.call(new TypeError('e')));
console.log(ts.call(new Boolean(false)));
console.log(ts.call(true));
console.log(ts.call(7));
console.log(ts.call(new String('ab')));
console.log(ts.call('ab'));
console.log(ts.call(/a/g));
console.log(ts.call({}));
// A chain that ends immediately: step 15's Get finds nothing to walk to, which
// is the same `undefined` a full walk produces, so the builtin tag stands.
console.log(ts.call(Object.create(null)));

// The ordinary spelling, reached through the prototype chain rather than
// through `.call` — `Object.prototype` is a real object on it (20.1.3.1).
console.log({}.toString());

// 20.1.3.5.
console.log({}.toLocaleString());
const overridden = { toString() { return 'mine'; } };
console.log(overridden.toString());
console.log(overridden.toLocaleString());
