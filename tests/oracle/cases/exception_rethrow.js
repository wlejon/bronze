// Rethrow: a caught value thrown again is the SAME value (ECMA-262 14.14.1:
// `throw` throws the value of its expression; nothing copies it), so object
// identity, own properties and the captured stack survive any number of hops.

function thrower() {
  var e = new TypeError("original");
  e.tag = 1;
  throw e;
}

function rethrowOnce() {
  try {
    thrower();
  } catch (e) {
    e.tag++;
    throw e;
  }
}

function rethrowTwice() {
  try {
    rethrowOnce();
  } catch (e) {
    e.tag++;
    throw e;
  }
}

var first = null;
try {
  thrower();
} catch (e) {
  first = e;
}

try {
  rethrowTwice();
} catch (e) {
  console.log(e instanceof TypeError, e.message, e.tag);
  console.log(e !== first);
  // The stack was captured once, at construction, and names the constructor
  // site, not a rethrow site.
  console.log(typeof e.stack === "string" && e.stack.indexOf("thrower") >= 0);
}

// Rethrowing a primitive.
function rethrowPrimitive() {
  try {
    throw 42;
  } catch (v) {
    throw v + 1;
  }
}
try {
  rethrowPrimitive();
} catch (v) {
  console.log(v);
}

// A rethrow from a nested catch inside a catch: the inner handler catches the
// new throw, the outer binding is untouched, and then the outer value leaves.
function nestedRethrow() {
  try {
    throw "outer";
  } catch (a) {
    try {
      throw "inner";
    } catch (b) {
      console.log("inner caught " + b + ", outer still " + a);
    }
    throw a;
  }
}
try {
  nestedRethrow();
} catch (v) {
  console.log("got " + v);
}

// A catch that rethrows only some values: the rest are handled.
function selective(v) {
  try {
    throw v;
  } catch (e) {
    if (typeof e === "number") return "handled " + e;
    throw e;
  }
}
console.log(selective(7));
try {
  selective("str");
} catch (e) {
  console.log("rethrown " + e);
}

// Rethrow through a finally: the finally runs and the value continues.
function rethrowThroughFinally(log) {
  try {
    try {
      throw new RangeError("r");
    } catch (e) {
      log.push("catch");
      throw e;
    } finally {
      log.push("finally");
    }
  } catch (e) {
    log.push(e.name + ":" + e.message);
  }
  return log.join(",");
}
console.log(rethrowThroughFinally([]));

// Many rethrows in a loop keep the handler state sound.
var count = 0;
for (var i = 0; i < 1000; i++) {
  try {
    try {
      throw i;
    } catch (v) {
      throw v * 2;
    }
  } catch (w) {
    count += w;
  }
}
console.log(count);
