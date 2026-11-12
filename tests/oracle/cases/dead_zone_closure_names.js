// CLOSURES IN THE PREFIX of the dead-zone analysis.
//
// `dead_zone_reach_boundary.js` beside this file pins the reach: the scan
// walks past a statement that names no binding still in its dead zone. A
// statement that builds a function used to stop it on sight; now the function
// is judged as the rest of the statement is, by the names under it, since a
// closure over a record reads the record's bindings by naming them and the
// mention walk descends into its body. `[0, 1].map((h) => make(h))` in front
// of a function's declarations names none of them and delays none of them.
//
// What that adds is a reader that outlives its statement. A closure made in
// the prefix names what it names for as long as it lives, and a name that was
// harmless when it was made can come to hold a closure over a dead zone
// afterwards. Every case below is one claim: what must still throw, or what
// must not.

function tryIt(fn) {
  try {
    return `ok:${fn()}`;
  } catch (e) {
    return e.name;
  }
}

function twice(x) {
  return x * 2;
}

// 1. The payoff: a callback in the first statement, and declarations below it
//    that a closure returned at the end reads. Nothing may throw.
function harmlessCallback() {
  const scaled = [1, 2, 3].map((x) => twice(x));
  const W = scaled.length, H = scaled[2];
  const items = [];
  for (let i = 0; i < W; i++) items.push(i + H);
  return {
    sum() {
      let s = 0;
      for (let i = 0; i < items.length; i++) s += items[i] + W;
      return s;
    },
  };
}

// 2. The callback names a binding declared below it and runs at once.
function callbackNamesLater() {
  const r = tryIt(() => [1].map(() => late)[0]);
  let late = "late";
  return `${r}/${late}`;
}

// 3. A closure stored on an object names a `var`. The `var` is harmless when
//    the closure is made and holds a closure over a dead zone when it is
//    called: the declaration that fills it is what has to stop the scan.
function storedNamesLaterVar() {
  const o = { m: () => h() };
  var h = function () { return late; };
  const r = tryIt(o.m);
  let late = "stored";
  return `${r}/${late}`;
}

// 4. The same through a `function` declaration nested in a block, which the
//    scan never stopped at.
function nestedDeclNamesLaterVar() {
  var keep;
  {
    function grab() { return h(); }
    keep = grab;
  }
  var h = function () { return late; };
  const r = tryIt(keep);
  let late = "nested";
  return `${r}/${late}`;
}

// 5. The `var` is filled by an assignment instead: the assignment names the
//    dead binding itself.
function storedNamesAssignedVar() {
  var h;
  const o = { m: () => h() };
  h = function () { return late; };
  const r = tryIt(o.m);
  let late = "assigned";
  return `${r}/${late}`;
}

// 6. Two hops: the callback calls one of this list's hoisted functions, which
//    reads the binding. The hoisted name never leaves the stop set.
function callbackCallsHoisted() {
  const r = tryIt(() => [1].map(() => peek())[0]);
  let hop = "hoisted";
  function peek() { return hop; }
  return `${r}/${hop}`;
}

// 7. A closure that names only what is above it, called below a declaration
//    it never names: both run, neither throws.
function closureNamesEarlier() {
  const base = 10;
  const add = [1, 2].map((x) => x + base);
  let total = add[0] + add[1];
  const bump = () => { total += base; return total; };
  return `${bump()}/${bump()}`;
}

// 8. An immediately invoked function in the prefix, harmless, then a getter
//    made below the declarations it reads.
function iifeThenGetter() {
  const seed = (function () { return 7; })();
  const k = seed * 3;
  const o = { get v() { return k + seed; } };
  return o.v;
}

// 9. A class expression whose method names a later binding, built in the
//    prefix and entered there.
function classExprNamesLater() {
  const C = class { m() { return cLate; } };
  const r = tryIt(() => new C().m());
  let cLate = "class";
  return `${r}/${cLate}`;
}

// 10. A closure made in a loop body in the prefix, kept, and called after the
//     binding it names is initialized: no throw, and the value is the
//     initialized one.
function keptClosureCalledLate() {
  const fns = [];
  for (let i = 0; i < 2; i++) fns.push(() => i + 100);
  const after = fns[0]() + fns[1]();
  const reader = () => after;
  return reader();
}

console.log(harmlessCallback().sum());
console.log(callbackNamesLater());
console.log(storedNamesLaterVar());
console.log(nestedDeclNamesLaterVar());
console.log(storedNamesAssignedVar());
console.log(callbackCallsHoisted());
console.log(closureNamesEarlier());
console.log(iifeThenGetter());
console.log(classExprNamesLater());
console.log(keptClosureCalledLate());
