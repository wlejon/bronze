// The errors the RUNTIME raises are ordinary catchable exceptions, not
// process-fatal diagnostics (docs/0020 decision 6). Before this they killed
// the process, which made every one of them unobservable from JS.
//
// Only the CLASS of each error is pinned, never its message: ECMA-262 fixes
// which constructor is used and says nothing about the text, so the text is
// bronze's choice and belongs in bronze's own tests. The clause that fixes
// the class is cited on each line.
//
// The other half of what this case pins is that execution CONTINUES after
// each one: every line below runs.

function classOf(f) {
  try {
    f();
    return "no-throw";
  } catch (err) {
    return err.name + "/" + (typeof err.message === "string" ? "msg" : "nomsg");
  }
}

// 7.2.1 RequireObjectCoercible, reached by 13.3.2.1 (MemberExpression . name)
// through GetValue: a TypeError for both nullish values, on read and write.
console.log(classOf(function () {
  var o = null;
  return o.x;
}));
console.log(classOf(function () {
  var o = undefined;
  return o.x;
}));
console.log(classOf(function () {
  var o = null;
  o.x = 1;
  return o;
}));
console.log(classOf(function () {
  var o = undefined;
  return o[0];
}));

// 7.3.14 Call step 1: "if IsCallable(F) is false, throw a TypeError".
console.log(classOf(function () {
  var n = 5;
  return n();
}));

// 7.3.15 Construct, reached by 13.3.5.1 (`new`) after IsConstructor fails.
console.log(classOf(function () {
  var n = 5;
  return new n();
}));

// 13.10.2 InstanceofOperator step 2, and the `in` production above it: both
// require an object on the right and throw a TypeError otherwise.
console.log(classOf(function () {
  return {} instanceof 5;
}));
console.log(classOf(function () {
  return "a" in 5;
}));

// 23.1.3.24 Array.prototype.reduce step 6: an empty array with no initial
// value is a TypeError, and step 2 makes a non-callable argument one too.
console.log(classOf(function () {
  return [].reduce(function (a, b) {
    return a + b;
  });
}));
console.log(classOf(function () {
  return [1, 2].forEach(7);
}));

// 22.1.3.16 String.prototype.repeat step 4: a negative count is a RangeError,
// which is the one place in this case that is NOT a TypeError.
console.log(classOf(function () {
  return "x".repeat(-1);
}));

// 7.3.20 RequireObjectCoercible, reached by 8.6.2 BindingInitialization: a
// destructuring whose source is nullish is a TypeError, and 7.4.2 GetIterator
// makes an array pattern or a spread over a non-iterable one too.
console.log(classOf(function () {
  var o = null;
  var [a] = o;
  return a;
}));
console.log(classOf(function () {
  var o = null;
  var { b } = o;
  return b;
}));
console.log(classOf(function () {
  var n = 5;
  var [a] = n;
  return a;
}));
console.log(classOf(function () {
  var n = 5;
  return [...n];
}));

// A runtime error unwinds like any other throw: out of a nested call, through
// the finallys on the way, into the outermost handler.
var trace = [];
function deepest() {
  var o = null;
  return o.boom;
}
function middle() {
  try {
    return deepest();
  } finally {
    trace.push("middle-fin");
  }
}
function outermost() {
  try {
    try {
      return middle();
    } finally {
      trace.push("outer-fin");
    }
  } catch (err) {
    trace.push("caught:" + err.name);
  }
  return trace.join(",");
}
console.log(outermost());

// And a runtime error raised inside an Array.prototype callback stops the
// iteration rather than running the remaining elements.
var seen = [];
try {
  [1, 2, 3].forEach(function (n) {
    seen.push(n);
    if (n === 2) {
      var o = null;
      o.x = 1;
    }
  });
} catch (err) {
  seen.push(err.name);
}
console.log(seen.join(","));
console.log("end");
