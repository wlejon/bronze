// A top level that awaits a timer, awaits again, and then rejects, with a
// global property write left after the `throw` where nothing reaches it (bro's
// tla_rejects_after_timer fixture). The top level is a coroutine body with
// speculation sites; the unreachable write's guard has no provable state
// once brass lowers the body (its resume target returns, which then reads
// the frame), so lowering must drop it rather than fail the compile.
// `bronze run` has no host timers, so `timer(n)` settles n jobs later, after
// the continuation that awaits it has been queued, as a timer would.
const state = { done: false, steps: 0 };

function timer(n) {
  let p = Promise.resolve();
  for (let i = 0; i < n; i++) p = p.then(() => {});
  return new Promise((resolve) => { p.then(resolve); });
}

function bump(o) { o.steps = o.steps + 1; return o.steps; }

async function failLater(n, why) {
  await timer(n);
  bump(state);
  throw new Error(why);
  state.done = true;
}

await timer(10);
console.log('resumed after timer', bump(state));
await null;
console.log('after second await', bump(state));

try {
  await timer(5);
  bump(state);
  throw new Error('rejected at the top level');
  globalThis.tlaFlag = true;
} catch (e) {
  console.log('caught:', e.message, state.steps, state.done, globalThis.tlaFlag);
}

let total = 0;
for (let i = 0; i < 2000; i++) total += bump(state) % 7;
console.log('hot loop', total);

try {
  await failLater(5, 'rejected after a timer');
  state.done = true;
} catch (e) {
  console.log('caught:', e.message, state.steps, state.done);
}

await failLater(1, 'final rejection').catch((e) => console.log('caught:', e.message, state.done));
console.log('end', state.steps);
