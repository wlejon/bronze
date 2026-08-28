// A GENERATOR and an ASYNC function whose bodies are full of promotable
// arithmetic, and which the entry region must refuse.
//
// It is refused by NAME — `il::Function::isResumeBody` — and not by shape,
// because the shape does not say it. A generator's body is lowered into a
// resume function whose entry block dispatches on an index held in the frame,
// so every block is reachable from `b0` and "no edge from outside enters
// anywhere but the entry" is TRUE of it. What is wrong with duplicating it is
// that its live values cross a suspension in the FRAME rather than in SSA: a
// promoted double cannot survive a yield, so the copy would be pure growth.
//
// The case is here because a refusal is invisible from the outside — a program
// that is still correct is what both a refusal and a wrong duplication would
// look like on the day the duplication happened to be harmless. These bytes are
// what a wrong one would have to keep producing.

function* gen(a, b) {
  const p = a * b;
  yield p;
  const q = a - b;
  yield q + p;
  return a / b;
}

// 3 * 4 is 12; (3 - 4) + 12 is 11. The `return` value is not yielded, so
// `for...of` sees two values (27.5.1.2 stops at `done`).
for (const v of gen(3, 4)) {
  console.log(v);
}

// The same body with a String operand: 13.15.3 coerces it, so 2 * "5" is 10 and
// (2 - "5") + 10 is 7.
for (const v of gen(2, '5')) {
  console.log(v);
}

// `throw` from a `valueOf` inside a generator: the coercion runs where the
// original body put it, which is inside the resume function.
const boom = {
  valueOf() {
    throw new RangeError('nope');
  },
};

try {
  for (const v of gen(boom, 2)) {
    console.log(v);
  }
  console.log('no throw');
} catch (e) {
  console.log(e instanceof RangeError);
}

async function asyncMath(a, b) {
  const p = a * b;
  const q = a - b;
  return p + q + a * 2 - b * 3;
}

console.log('sync-done');

// 30 + (-1) + 10 - 18 is 21. The continuation runs in the microtask the promise
// resolution queues, which is after the last synchronous line above.
asyncMath(5, 6).then(function (v) {
  console.log(v);
});
