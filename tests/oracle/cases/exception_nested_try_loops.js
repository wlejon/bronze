// Nested try statements inside loops: each throw lands in the innermost
// handler that encloses it, handlers are re-entered on every iteration, and a
// value that escapes an inner handler reaches the next one out.

function thrower(v) {
  throw v;
}

function nestedInLoop(n) {
  var log = [];
  for (var i = 0; i < n; i++) {
    try {
      for (var j = 0; j < 3; j++) {
        try {
          if (j === 1) thrower("inner" + i);
          if (i === 2 && j === 2) thrower("outer" + i);
          log.push(i + "." + j);
        } catch (e) {
          if (e.indexOf("inner") !== 0) throw e;
          log.push("ci" + i);
        }
      }
    } catch (e) {
      log.push("co:" + e);
    }
  }
  return log.join(" ");
}
console.log(nestedInLoop(4));

// Three levels, each with a finally, broken out of from the middle.
function threeLevels() {
  var log = [];
  outer: for (var a = 0; a < 2; a++) {
    try {
      for (var b = 0; b < 2; b++) {
        try {
          for (var c = 0; c < 2; c++) {
            try {
              if (a === 1 && b === 0 && c === 1) thrower("x");
              if (a === 1 && b === 1) break outer;
              log.push("" + a + b + c);
            } finally {
              log.push("f3");
            }
          }
        } catch (e) {
          log.push("c2:" + e);
        } finally {
          log.push("f2");
        }
      }
    } finally {
      log.push("f1");
    }
  }
  return log.join(",");
}
console.log(threeLevels());

// A while loop whose condition is computed in a try, with the catch deciding
// whether to continue.
function retryLoop() {
  var attempts = 0;
  var result = null;
  while (result === null) {
    try {
      attempts++;
      if (attempts < 4) thrower(attempts);
      result = "ok after " + attempts;
    } catch (v) {
      if (v > 10) break;
    }
  }
  return result;
}
console.log(retryLoop());

// A handler inside a loop inside a handler, where the outer catch binding is
// shadowed by the inner one.
function shadowed() {
  var out = [];
  try {
    thrower("A");
  } catch (e) {
    for (var i = 0; i < 2; i++) {
      try {
        thrower("B" + i);
      } catch (e) {
        out.push(e);
      }
    }
    out.push(e);
  }
  return out.join(",");
}
console.log(shadowed());

// Values live across the handler keep the value they had at the throw.
function liveAcross(n) {
  var sum = 0;
  var k = 0;
  for (var i = 0; i < n; i++) {
    var before = sum;
    try {
      sum += i;
      k = i * 2;
      if (i % 3 === 0) thrower(i);
      sum += 1;
    } catch (v) {
      if (sum !== before + v) return "bad at " + i;
      if (k !== v * 2) return "bad k at " + i;
    }
  }
  return sum + " " + k;
}
console.log(liveAcross(30));

// Many iterations with a throw in each, deep in nested handlers.
function stress(n) {
  var c = 0;
  for (var i = 0; i < n; i++) {
    try {
      try {
        try {
          thrower(i);
        } catch (v) {
          if (v % 2) throw v;
          c += 1;
        }
      } catch (v) {
        if (v % 3) throw v;
        c += 10;
      }
    } catch (v) {
      c += 100;
    }
  }
  return c;
}
console.log(stress(3000));
