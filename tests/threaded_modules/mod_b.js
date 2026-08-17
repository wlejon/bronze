// Module B of the threaded-modules test: the same shape as module A with
// different constants and one extra piece — a module-scope closure pair — so
// the two runtimes' answers cannot be mistaken for each other. Runs on a
// std::thread concurrently with A on the main thread; the entry's first
// allocation is what builds this thread's runtime.

const parts = [];
for (let i = 0; i < 120; i++) parts.push("b" + i);

class Counter {
  constructor() { this.n = 0; }
  add(k) { this.n += k; return this.n; }
}
const counter = new Counter();
for (let i = 1; i <= 60; i++) counter.add(i);

// A closure over module scope, read back through the summary: the module
// environment cell is one of the .data spans the entry registered with THIS
// thread's collector, and this is what notices if it registered with the
// wrong one.
let secret = "B-" + 6 * 7;
function readSecret() { return secret; }

globalThis.summaryB = "parts=" + parts.length + " last=" + parts[parts.length - 1] +
                      " count=" + counter.n + " secret=" + readSecret();

globalThis.hammerB = function (i) {
  const items = [];
  for (let j = 0; j < 30; j++) items.push("B" + i + ":" + j);
  return "B" + i + "#" + items.length + "#" + items[items.length - 1];
};
