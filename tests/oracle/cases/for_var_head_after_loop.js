// A `var` head in for-in / for-of declares the FUNCTION's binding: it is
// hoisted (undefined before the loop), every iteration assigns the one
// binding, closures over it share it, and code after the loop reads the last
// value it was given, or undefined when the loop never ran.

function g(x) {}

function afterBoth(o, flag) {
  for (var k in o) {
    if (k === "zz") g(k);
  }
  if (flag) {
    for (var k of [1, 2]) g(k);
  }
  return k;
}

function sharedByClosures() {
  const fns = [];
  for (var k of [1, 2, 3]) fns.push(() => k);
  return fns.map(f => f()).join(",") + ":" + k;
}

function destructured() {
  for (var [x, y] of [[1, 2], [3, 4]]) {}
  return x + y;
}

function brokenOut(o) {
  let n = 0;
  for (var k in o) {
    if (k === "b") break;
    n++;
  }
  return k + n;
}

function* inGenerator() {
  for (var v of [5, 6]) yield v;
  yield v * 10;
}

function neverRan() {
  var seen = typeof k;
  for (var k in {}) {}
  return seen + ":" + k;
}

// A head naming an existing binding assigns it the same way.
function existingBinding(o) {
  let k = 0;
  for (k in o) {}
  return k + 1;
}

console.log(existingBinding({ p: 1, q: 2 }));
console.log(afterBoth({ a: 1, b: 2 }, false));
console.log(afterBoth({ a: 1, b: 2 }, true));
console.log(sharedByClosures());
console.log(destructured());
console.log(brokenOut({ a: 1, b: 2, c: 3 }));
console.log([...inGenerator()].join(","));
console.log(neverRan());
