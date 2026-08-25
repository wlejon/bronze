// Environment-record ACCESS through the inline path, at the shapes stage E2
// changed: the guard's failure edge no longer returns, so a read is a load and
// a write is a store with nothing merging behind them. What must survive that
// is everything the guards were never about — the TDZ, which is 9.1.1.1.6 and
// not a tripwire, and the chain walk at depth > 0.
//
// The interesting case is a CLOSURE reading a captured `let` before the
// declaration that initializes it has run. The read is in a different function
// from the write, so no dominance argument inside either one can see it, and
// the marker compare on the slot is the whole of what makes it a
// ReferenceError rather than a read of an internal sentinel.

function factory() {
  const peek = () => early;      // captures a slot still in its dead zone
  const poke = (v) => { early = v; };
  let escaped = null;
  try {
    peek();
  } catch (e) {
    escaped = `${e.name}: ${e.message}`;
  }
  let writeFailed = null;
  try {
    poke(1);
  } catch (e) {
    writeFailed = `${e.name}: ${e.message}`;
  }
  let early = 41;
  poke(early + 1);
  return [escaped, writeFailed, peek()];
}
console.log(factory().join(" | "));

// The same across a REAL closure boundary: the reader escapes the scope and is
// called from outside it, before and after the initializer.
function makeReader() {
  const read = () => late;
  const results = [];
  try { read(); } catch (e) { results.push(e.name); }
  let late = "ready";
  results.push(read());
  return [read, results];
}
const [reader, phases] = makeReader();
console.log(phases.join(","), reader());

// Depth: three nested scopes, each contributing captured slots, read and
// written from the innermost closure. The chain walk is one parent load per
// level, and every level's brand is checked.
function depth3() {
  let a = 1;
  {
    let b = 10;
    {
      let c = 100;
      const bump = (n) => { a += n; b += n * 2; c += n * 3; return a + b + c; };
      const out = [bump(1), bump(2), bump(3)];
      return out.join(",") + " / " + [a, b, c].join(",");
    }
  }
}
console.log(depth3());

// Per-iteration records: each round of the loop gets its OWN record, so the
// closures captured in different iterations must see different slots — and a
// closure captured in iteration i must not observe iteration i+1's record.
const captured = [];
for (let i = 0; i < 4; i++) {
  let doubled = i * 2;
  captured.push(() => `${i}:${doubled}`);
  doubled += 1;
}
console.log(captured.map((f) => f()).join(" "));

// A `const` slot read through a closure, and its dead zone: `const` is lexical
// too, so the same marker compare answers both. The WRITE half — what a
// closure assigning to a `const` binding must do — is pinned in
// cases/blocked/const_reassignment.js instead, because bronze gets it wrong
// today and has since before this file existed.
function constSlot() {
  const read = () => fixed;
  const out = [];
  try { read(); } catch (e) { out.push(e.name); }
  const fixed = 7;
  out.push(read());
  out.push(read());
  return out.join(",");
}
console.log(constSlot());

// Writes THROUGH a closure interleaved with reads through another, so a slot's
// value has to be re-read rather than remembered across the call that changed
// it. This is the property the stage's alias/merge work must not break.
function interleaved() {
  let n = 0;
  const inc = () => { n = n + 1; };
  const get = () => n;
  const seen = [];
  for (let i = 0; i < 5; i++) {
    seen.push(get());
    inc();
    seen.push(get());
    inc();
  }
  return seen.join("");
}
console.log(interleaved());

// A slot whose value is an object, so the collector may MOVE it between the
// store and the next read: the read has to come back through the record, not
// out of a register holding a stale address.
function movingSlot() {
  let boxv = { n: 0 };
  const bump = () => { boxv = { n: boxv.n + 1, pad: new Array(8).fill(boxv.n) }; };
  const peek = () => boxv.n;
  let total = 0;
  for (let i = 0; i < 200; i++) {
    bump();
    total += peek();
  }
  return total;
}
console.log(movingSlot());
