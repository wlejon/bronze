// A global read loads its module cache cell inline and calls the slot's
// thunk only for a hole. Every way the cell can go stale has to reach the
// thunk: a global written, replaced, deleted, defined after a failed read.
function sum(n) { let s = 0; for (let k = 0; k < n; k++) s += Math.sqrt(k) + Math.abs(k - 5); return s; }
console.log("math", Math.round(sum(1000)));

// A global object replaced wholesale; its methods are read off the new one.
globalThis.Tools = { twice: (x) => 2 * x };
function useTools(n) { let s = 0; for (let k = 0; k < n; k++) s += Tools.twice(k); return s; }
console.log("tools", useTools(10));
globalThis.Tools = { twice: (x) => 3 * x };
console.log("replaced", useTools(10));

// A global that does not exist yet, then does, then is deleted.
function readLate() { return lateGlobal; }
for (let i = 0; i < 3; i++) {
    try { console.log("late", readLate()); } catch (e) { console.log("late", e instanceof ReferenceError); }
    if (i === 0) globalThis.lateGlobal = 42;
    if (i === 1) delete globalThis.lateGlobal;
}

// A global written from many places, read in a loop.
globalThis.counter = 0;
function bump() { globalThis.counter = counter + 1; }
function readCounter(n) { let s = 0; for (let i = 0; i < n; i++) { bump(); s += counter; } return s; }
console.log("counter", readCounter(100), counter);

// Reads of several globals in one expression and inside try.
function mix() {
    try { return typeof JSON + typeof Map + String(Number.MAX_SAFE_INTEGER) + parseInt("12px"); }
    catch (e) { return "threw"; }
}
console.log("mix", mix());

// A getter on the global object answers every read.
let hits = 0;
Object.defineProperty(globalThis, "ticker", { get() { return ++hits; }, configurable: true });
function readTicker(n) { let s = 0; for (let i = 0; i < n; i++) s += ticker; return s; }
console.log("ticker", readTicker(10), hits);
