// A JS callback invoked BY A BUILTIN, whose wrapper allocates before it has
// finished reading its arguments. This is the intersection nothing else in the
// suite covers, and it hid a silent wrong answer.
//
// A builtin builds its callback's argument block in plain stack memory
// (`Value block[3]` in builtin_array.cpp). Generated code's block lives in a GC
// root frame and is updated when the collector moves things; a builtin's is
// not. So when the callee's wrapper allocates — which it does for a rest
// parameter and for `arguments` — every argument it has not yet read is stale
// afterwards. A forwarded header keeps its tag, so the value still looks like
// an array or a string and reports a garbage length: a wrong answer, not a
// crash, which is why only `oracle-gc-stress` can see it.
//
// Every line here is derived from ECMA-262: 23.1.3.20 (map calls
// callbackfn(kValue, k, O) — three arguments), 23.1.3.15 (forEach, the same
// three), 23.1.3.24 (reduce, four: accumulator, kValue, k, O), 10.4.2 (an
// array's length), 6.1.4 (a String's length in code units) and 8.6.2 (a rest
// parameter takes every argument past the named ones).

const items = [{ n: 1 }, { n: 2 }, { n: 3 }];

// The element the builtin wrote into the block before the wrapper allocated.
console.log(items.map(function (...a) { return a[0].n; }).join(","));

// The third argument map passes is the array itself, so its length is 3 every
// time. This is the entry that read as a garbage length.
console.log(items.map(function (...a) { return a[2].length; }).join(","));

// A NAMED parameter read after the `arguments` object has been built. Same
// staleness, reached through the other of the wrapper's two allocations.
function withArgs(o) { return arguments.length + ":" + o.n; }
console.log(items.map(withArgs).join(","));

// Both allocations in one wrapper, with a named parameter to keep live across
// the pair: `rest` takes the index and the array, so its length is 2.
function mixed(o, ...rest) { return o.n + "/" + rest.length; }
console.log(items.map(mixed).join(","));

// A string element, so what moves is a StringHeader rather than an object.
const words = ["ab", "cde", "f"];
const seen = [];
words.forEach(function (...a) { seen.push(a[0].length); });
console.log(seen.join(","));

// reduce passes four, and the accumulator is a primitive while the element is
// not: 0+1 = 1, 1+2 = 3, 3+3 = 6.
console.log(items.reduce(function (...a) { return a[0] + a[1].n; }, 0));
