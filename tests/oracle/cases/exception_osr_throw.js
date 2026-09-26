// Throws that happen after a function has gone hot: a loop long enough to be
// entered on-stack by an optimized tier, with the throw arriving only late in
// the run, so the handler that catches it is in code that did not exist when
// the loop started. Each result is plain arithmetic and holds on every tier.

function hotThenThrow(n, at) {
  var acc = 0;
  try {
    for (var i = 0; i < n; i++) {
      acc = (acc + i * 7) % 1000003;
      if (i === at) throw new Error("late " + i);
    }
  } catch (e) {
    return e.message + " acc=" + acc;
  }
  return "no throw acc=" + acc;
}
console.log(hotThenThrow(300000, 250000));
console.log(hotThenThrow(1000, 5000));

// The throw comes out of a callee that is itself hot by then.
function maybeFail(i, at) {
  if (i === at) throw i;
  return i & 15;
}
function hotCalleeThrow(n, at) {
  var acc = 0;
  var i = 0;
  try {
    for (; i < n; i++) acc += maybeFail(i, at);
  } catch (v) {
    return "caught " + v + " after acc=" + acc;
  }
  return "done acc=" + acc;
}
console.log(hotCalleeThrow(400000, 399999));

// The handler is inside the loop: every iteration past the threshold throws,
// and the loop keeps going after each catch.
function catchEachIteration(n) {
  var caught = 0;
  var sum = 0;
  for (var i = 0; i < n; i++) {
    try {
      if (i >= n - 1000) throw i;
      sum += i & 3;
    } catch (v) {
      caught++;
    }
  }
  return caught + " " + sum;
}
console.log(catchEachIteration(300000));

// Nothing in this function catches: the throw leaves a hot frame and is
// caught by a cold caller.
function hotNoHandler(n) {
  var acc = 0;
  for (var i = 0; i < n; i++) {
    acc = (acc * 31 + i) | 0;
    if (i === n - 1) throw acc;
  }
  return acc;
}
try {
  hotNoHandler(200000);
} catch (v) {
  console.log("uncaught in hot frame: " + v);
}

// The same function called again after the throw, so the optimized code runs
// both the throwing and the non-throwing path.
var ok = 0;
for (var r = 0; r < 3; r++) {
  var out = hotThenThrow(100000, r === 1 ? 99999 : -1);
  if (out.indexOf("no throw") === 0) ok++;
}
console.log("ok " + ok);
