// A long-lived set built by a top-level loop, then batches of short-lived
// records timed one by one. The top level is one function run once, so only
// OSR compiles it; the first loop's entry is compiled before the batch loop
// has ever run, and a guard that code arms for a site it has never seen
// fails on the first batch, which then runs in the interpreter until the
// batch loop is compiled again.
const live = [];
for (let i = 0; i < 500000; i++) live.push({ a: i, b: [i, i + 1], c: 's' + (i % 100) });
const ds = [];
for (let b = 0; b < 200; b++) {
  const s = Date.now();
  let t = [];
  for (let j = 0; j < 20000; j++) t.push({ x: j, y: [j] });
  ds.push(Date.now() - s);
}
console.log('batches first=' + ds[0] + 'ms max-after=' + Math.max(...ds.slice(1)) + 'ms');
