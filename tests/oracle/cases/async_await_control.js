// `await` under CONTROL FLOW: in loops, inside `try`/`catch`/`finally`, and
// across nested async calls — plus a stage that keeps forty suspended async
// frames alive across a few hundred allocations, which is what this case
// contributes to the suite's BRONZE_GC_STRESS run.
//
// Every line of the expectation was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.7.5.3 Await: a suspension, resumed with either a normal completion
//   carrying the fulfilled value or a THROW completion carrying the rejection
//   reason — raised AT the await's own position. That single sentence is the
//   whole of `try { await p } catch (e)`: the throw happens inside the
//   protected region, so 14.15's ordinary rules take it from there.
// * 14.15.3 TryStatement: the Finally block runs for EVERY completion of the
//   Block — including the throw an await delivered, and including the `return`
//   written inside a `catch`. A `finally` that completes normally leaves the
//   original completion in place, so `cleanupOnly` still rejects.
// * 27.7.5.1: an uncaught throw in an async body rejects that body's promise,
//   and awaiting that promise re-raises the reason at the awaiting site — which
//   is how `propagated:` is reached from a `finally` that caught nothing.
// * 14.7 / 14.7.5: a loop body containing an await is suspended and resumed
//   once per iteration; the loop's own state lives in the frame across every
//   one of those suspensions.
// * 27.2.4.1 `Promise.all` resolves with the values in INPUT order, whatever
//   order the forty frames actually finished in.
//
// Ordering note, since it is the part most easily got wrong by hand: `sum` is
// called before it is awaited, so its first suspension happens while `nested`
// is still running — and `nested`'s own first line therefore prints only after
// `sum` has finished all three of its iterations.

async function sum(values) {
  let total = 0;
  for (const v of values) {
    total = total + (await v);
  }
  return total;
}

async function fails() {
  throw 'inner failure';
}

// The reason a `return` inside a `catch` is worth its own function: 14.15.3
// makes the `finally` run BEFORE the return completes, so 'finally ran' is
// printed while the return value is still in flight.
async function guarded() {
  try {
    await fails();
    console.log('unreachable');
  } catch (e) {
    console.log('caught: ' + e);
    return 'from catch';
  } finally {
    console.log('finally ran');
  }
}

// A `finally` with no `catch`: it observes the failure and lets it continue.
async function cleanupOnly() {
  try {
    await fails();
  } finally {
    console.log('cleanup ran');
  }
}

function makeGate() {
  let settle = null;
  const promise = new Promise(function (resolve) { settle = resolve; });
  return { promise: promise, settle: settle };
}

async function waiter(gate, index) {
  const label = 'waiter-' + index;
  const value = await gate.promise;
  return label + ':' + value;
}

// Forty async frames, all suspended at once, with allocation happening between
// their suspension and their resumption. Nothing on any stack holds any of
// them: each is reachable only through the reaction its `await` subscribed to
// its gate's promise. Under BRONZE_GC_STRESS every allocation below moves the
// whole live set, so this walks that root path a few hundred times.
async function manyFrames() {
  const gates = [];
  const pending = [];
  for (let i = 0; i < 40; i = i + 1) {
    const gate = makeGate();
    gates.push(gate);
    pending.push(waiter(gate, i));
  }
  let junk = '';
  for (let i = 0; i < 200; i = i + 1) {
    junk = 'junk-' + i;
  }
  for (let i = 0; i < gates.length; i = i + 1) {
    gates[i].settle('v' + i);
  }
  const results = await Promise.all(pending);
  return 'frames: ' + results.length + ' first=' + results[0] +
         ' last=' + results[39] + ' junk=' + junk;
}

async function nested() {
  const a = await sum([1, 2, 3]);
  console.log('sum = ' + a);

  const b = await guarded();
  console.log('guarded = ' + b);

  try {
    await cleanupOnly();
    console.log('unreachable');
  } catch (e) {
    console.log('propagated: ' + e);
  }

  let n = 0;
  let i = 0;
  while (i < 3) {
    n = n + (await (i + 1));
    i = i + 1;
  }
  console.log('while total = ' + n);
  return 'done';
}

console.log('sync start');

nested().then(function (r) {
  console.log('nested: ' + r);
  return manyFrames();
}).then(function (digest) {
  console.log(digest);
});

console.log('sync end');
