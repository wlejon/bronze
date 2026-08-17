// Module A of the threaded-modules test: compiled to its own object under its
// own entry symbol and run on the host's MAIN thread while module B runs on a
// std::thread — tests/threaded_modules/host.cpp says what that proves.
//
// Deliberately allocation-heavy (arrays, strings, class instances, a closure)
// so that under BRONZE_GC_STRESS every line here crosses a collection on THIS
// thread's heap while module B is doing the same on its own. And deliberately
// silent: two threads own one stdout, so the modules compute and the host
// prints after the join.

const parts = [];
for (let i = 0; i < 200; i++) parts.push("a" + i);

class Counter {
  constructor() { this.n = 0; }
  add(k) { this.n += k; return this.n; }
}
const counter = new Counter();
for (let i = 1; i <= 100; i++) counter.add(i);

globalThis.summaryA = "parts=" + parts.length + " last=" + parts[parts.length - 1] +
                      " count=" + counter.n;

// The hammer the host calls 64 times with a collection after each: a fresh
// burst of allocation whose result depends on all of it surviving, so a
// cross-thread rooting or forwarding mistake answers a wrong string rather
// than crashing quietly.
globalThis.hammerA = function (i) {
  const items = [];
  for (let j = 0; j < 40; j++) items.push("A" + i + ":" + j);
  return "A" + i + "#" + items.length + "#" + items[items.length - 1];
};
