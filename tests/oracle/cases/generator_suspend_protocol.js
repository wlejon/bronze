// Generators suspended inside try/finally and delegations: return() and
// throw() arriving mid-iteration run the finally blocks between the yield and
// the body's end, a catch sees a throw() at the yield it was suspended at, and
// yield* forwards next/throw/return to the inner iterator.

function* withFinally(log) {
  try {
    log.push("start");
    yield 1;
    log.push("after 1");
    yield 2;
    log.push("after 2");
  } finally {
    log.push("finally");
  }
  log.push("end");
}

{
  const log = [];
  const g = withFinally(log);
  console.log(JSON.stringify(g.next()));
  console.log(JSON.stringify(g.return(42)));
  console.log(JSON.stringify(g.next()));
  console.log(log.join(","));
}

{
  const log = [];
  const g = withFinally(log);
  g.next();
  g.next();
  try {
    g.throw(new Error("boom"));
  } catch (e) {
    console.log("outer caught", e.message);
  }
  console.log(JSON.stringify(g.next()));
  console.log(log.join(","));
}

{
  const log = [];
  for (const v of withFinally(log)) {
    if (v === 2) break;
  }
  console.log("break:", log.join(","));
}

// A finally that yields keeps the generator alive through return().
function* yieldingFinally() {
  try {
    yield "a";
  } finally {
    yield "cleanup";
    console.log("cleanup done");
  }
}
{
  const g = yieldingFinally();
  console.log(JSON.stringify(g.next()));
  console.log(JSON.stringify(g.return("r")));
  console.log(JSON.stringify(g.next()));
  console.log(JSON.stringify(g.next()));
}

// A finally that returns overrides return()'s value.
function* overridingFinally() {
  try {
    yield 1;
  } finally {
    return "from finally";
  }
}
{
  const g = overridingFinally();
  g.next();
  console.log(JSON.stringify(g.return("ignored")));
}

// throw() caught at the yield: the generator keeps going.
function* catcher() {
  let n = 0;
  while (true) {
    try {
      const got = yield n;
      console.log("got", got);
    } catch (e) {
      console.log("caught", e);
      n += 10;
    }
    n++;
  }
}
{
  const g = catcher();
  console.log(g.next().value);
  console.log(g.next("x").value);
  console.log(g.throw("t1").value);
  console.log(g.next("y").value);
  console.log(JSON.stringify(g.return("stop")));
}

// Nested try/finally: return() unwinds every level, innermost first.
function* nested() {
  try {
    try {
      yield "inner";
    } finally {
      console.log("inner finally");
    }
  } finally {
    console.log("outer finally");
  }
}
{
  const g = nested();
  g.next();
  console.log(JSON.stringify(g.return(7)));
}

// yield*: next values, sent values and return/throw reach the inner one.
function* inner() {
  try {
    const a = yield "i1";
    console.log("inner got", a);
    const b = yield "i2";
    console.log("inner got", b);
    return "inner result";
  } finally {
    console.log("inner finally");
  }
}
function* outer() {
  const r = yield* inner();
  console.log("delegate returned", r);
  yield "o1";
}
{
  const g = outer();
  console.log(JSON.stringify(g.next()));
  console.log(JSON.stringify(g.next("A")));
  console.log(JSON.stringify(g.next("B")));
  console.log(JSON.stringify(g.next()));
}
{
  const g = outer();
  g.next();
  console.log(JSON.stringify(g.return("early")));
}
{
  const g = outer();
  g.next();
  try {
    g.throw("into inner");
  } catch (e) {
    console.log("rethrown", e);
  }
}

// yield* over a plain iterable and over a generator that catches throw().
function* forgiving() {
  while (true) {
    try {
      yield "f";
    } catch (e) {
      console.log("forgave", e);
    }
  }
}
function* wraps() {
  yield* [1, 2];
  yield* forgiving();
}
{
  const g = wraps();
  console.log(g.next().value, g.next().value, g.next().value);
  console.log(g.throw("err").value);
  console.log(JSON.stringify(g.return("bye")));
}

// A yield inside an expression keeps the partial operands.
function* inExpr() {
  const parts = ["p", yield 1, "q", yield 2];
  return parts.join("") + (yield 3) * 2;
}
{
  const g = inExpr();
  g.next();
  g.next("X");
  g.next("Y");
  console.log(JSON.stringify(g.next(21)));
}

// Parameters, arguments and closures survive suspension.
function* params(a, b = a * 2, ...rest) {
  const f = () => a + b + rest.length;
  yield f();
  a = 100;
  yield f();
  yield arguments.length;
}
console.log([...params(1, undefined, 5, 6)].join(","));
