// `for (let i = …)` beside a closure that has an `i` of its own — the shapes
// that share a SPELLING with the per-iteration binding problem and nothing
// else (ECMA-262 14.7.4.9 CreatePerIterationEnvironment).
//
// A `let` loop binding is copied per iteration, so a closure that reaches it
// must capture that iteration's copy (pinned as
// `for_loop_per_iteration_binding`) — and a loop no closure reaches must not
// pay for the copies, which is what these pin. The test for WHEN it
// matters used to be "does any closure anywhere in this function mention this
// name", which is not the same question: a callback with a parameter called
// `i` never touches the loop's `i` at all. `for (let i = 0, il = a.length; i <
// il; i++)` beside `a.map((x, i) => …)` is ubiquitous, so that answer rejected
// a large fraction of ordinary JavaScript.
//
// What this pins — every one of these must COMPILE, and to these values:
//
// 1. A closure whose PARAMETER is the loop binding's name. The parameter
//    shadows for the whole body, so the closure's `v` is the argument.
// 2. The three.js shape: the two-declarator head next to an `Array.prototype`
//    callback whose own index parameter is called `i`.
// 3. A closure that declares the name itself — with `let`, with `var`, or
//    two closures down where the inner one's parameter shadows for it. Only
//    declarations that cover the closure's WHOLE body count: a `var` written
//    inside a nested block is function-scoped by 8.6.2 but bronze does not
//    hoist it out of that block, so such a closure is treated as reaching the
//    loop's binding rather than a shadow that would not exist — the copies get
//    built, and the answer stays right either way.
// 4. A closure OUTSIDE the loop over an enclosing binding the loop's own
//    declaration shadows. Its `v` is the outer one, and the loop must not
//    reuse the outer binding's storage for the inner declaration: the answer
//    is 99, never 3.
// 5. `var` in the head, which is not a lexical declaration at all — 14.7.4.9
//    copies nothing, so there is ONE binding for the whole loop and every
//    closure over it sees the value the loop left behind.

// 1. A parameter of the same name.
function shadowedByParam() {
  const out = [];
  for (let v = 0; v < 2; v++) {
    out.push("for-" + v);
  }
  const g = (v) => "arrow-" + v;
  out.push(g("a"));
  return out.join(",");
}
console.log(shadowedByParam());

// 2. The head with two declarators, next to a callback taking an index.
function indexedWalk(items) {
  let total = 0;
  for (let i = 0, il = items.length; i < il; i++) {
    total += items[i];
  }
  const labels = items.map(function (x, i) {
    return i + ":" + x;
  });
  return total + "|" + labels.join(",");
}
console.log(indexedWalk([10, 20, 30]));

// 3a. A closure whose own `let` binds the name.
function shadowedByLet() {
  const out = [];
  for (let i = 0; i < 2; i++) {
    out.push(
      (function () {
        let i = 7;
        return i;
      })()
    );
  }
  return out.join(",");
}
console.log(shadowedByLet());

// 3b. A closure whose own `var` binds the name.
function shadowedByVar() {
  let out = "";
  for (let i = 0; i < 2; i++) {
    out += (function () {
      var i = 5;
      return i;
    })();
  }
  return out;
}
console.log(shadowedByVar());

// 3c. Two levels down: the inner closure's own parameter shadows for it too.
function shadowedTwoDeep() {
  const out = [];
  for (let i = 0; i < 2; i++) {
    out.push(
      (function (i) {
        return function () {
          return i * 2;
        };
      })(i + 10)()
    );
  }
  return out.join(",");
}
console.log(shadowedTwoDeep());

// 4. A closure outside the loop over the binding the loop shadows.
function shadowsAnOuterCapture() {
  let v = 99;
  const read = () => v;
  for (let v = 0; v < 3; v++) {
    // nothing; the loop exists only to declare `v`
  }
  return read();
}
console.log(shadowsAnOuterCapture());

// 5. `var` in the head: one binding, captured by all three closures.
function varHead() {
  const fns = [];
  for (var i = 0; i < 3; i++) {
    fns.push(function () {
      return i;
    });
  }
  return [fns[0](), fns[1](), fns[2](), i].join(",");
}
console.log(varHead());
