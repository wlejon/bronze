// IteratorClose (ECMA-262 7.4.9), the half of the iterator protocol
// docs/0020 could not have: a `for-of` abandoned before its iterator says
// `done` must call the iterator's `return` method, and an exhausted one must
// not. It becomes observable only now that an iterator can be a user object
// (docs/0021 decision 3), which is why it is pinned in its own case.
//
// The iterator below reports the cursor position it was closed AT, so "was
// `return` called" and "when" are one observation. Nothing here is timing or
// identity dependent, so the inference and --no-infer runs must agree.
//
// 1. `break` closes. So does `return` out of the enclosing function, and so
//    does a `throw` from the body — 14.7.5.6 routes all three through
//    IteratorClose, and the throw case then carries the ORIGINAL exception
//    rather than anything `return` produced (7.4.9 step 6).
// 2. Running to exhaustion does NOT close: 7.4.9 is reached only for an
//    iteration abandoned early, so the fourth loop logs nothing.
// 3. A `finally` INSIDE the loop body runs before the close, because the
//    body's completion is what the loop then acts on; a `finally` OUTSIDE
//    the loop runs after it. The two orderings are the reason the loop's
//    cleanup is a frame of its own rather than a finally.
// 4. Array destructuring closes too (8.6.2 step 5) — `const [p, q] = it`
//    stops with the iterator un-exhausted — but a REST element drains it, so
//    there is nothing left to close and `return` is not called.

let log = "";
function counted(tag) {
  let i = 0;
  return {
    [Symbol.iterator]: function () {
      return {
        next: function () {
          i = i + 1;
          return { value: i, done: i > 5 };
        },
        return: function () {
          log = log + tag + ":close@" + i + ";";
          return { done: true };
        },
      };
    },
  };
}

for (const v of counted("a")) {
  if (v === 2) break;
}
console.log(log);

log = "";
for (const v of counted("b")) {
  try {
    if (v === 2) break;
  } finally {
    log = log + "body-finally;";
  }
}
console.log(log);

log = "";
try {
  for (const v of counted("c")) {
    if (v === 3) break;
  }
} finally {
  log = log + "outer-finally;";
}
console.log(log);

log = "";
for (const v of counted("d")) {
  log = log + v + ";";
}
console.log(log);

log = "";
function pick() {
  for (const v of counted("e")) {
    if (v === 2) return "took" + v;
  }
  return "none";
}
console.log(pick(), log);

log = "";
try {
  for (const v of counted("f")) {
    if (v === 2) throw new Error("boom");
  }
} catch (e) {
  log = log + "caught:" + e.message + ";";
}
console.log(log);

log = "";
const [p, q] = counted("g");
console.log(p, q, log);

log = "";
const [r, ...more] = counted("h");
console.log(r, more.join(","), "log=" + log);

// A FAST kind has no `return` to call, so an abandoned walk over an array, a
// string, a Set or a Map closes by doing nothing at all — the same
// instruction, and no call into user code.
let short = "";
for (const c of "abcdef") {
  if (c === "c") break;
  short = short + c;
}
for (const n of [1, 2, 3, 4]) {
  if (n === 3) break;
  short = short + n;
}
for (const s of new Set(["x", "y", "z"])) {
  if (s === "y") break;
  short = short + s;
}
console.log(short);
