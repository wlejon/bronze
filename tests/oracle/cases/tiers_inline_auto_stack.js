// Guarded method inlining in the tiered engine (lower/inline_guarded.h): the
// method calls of drive's loop run as bodies compiled into drive, first in
// Tier 0, then in drive's tier-1 code and its tier-2 / OSR code as the loop
// gets hot. Every frame that runs such a body reports the callee frames it
// stands for, so a stack taken inside Leaf.read (new Error().stack) and an
// exception thrown out of it read the same at every point of the run: each
// set below holds one string. Before the second run Math.floor is replaced
// (by a function of the same result): the OSR code's call-target guard on it,
// inside the inlined body of read, fails, the frame finishes in Tier 0 and
// OSRs again, and deoptimizes again, and the stacks taken in all that still
// name each function once: a frame that deoptimized is not reported beside
// the Tier-0 frame finishing it (brass_native_frame_deoptimized). The odd
// operand at i == 1 (another shape, the same x) gives drive's copy of the p.x
// read a miss of its own, which tier 2 may arm as a guard, and the second
// run's every-eighth P2 fails it.

function frames(stack) {
  const out = [];
  for (const line of stack.split('\n').slice(1)) {
    const m = line.match(/at (?:(\S+) \()?.*?([^\/\\]+\.js):(\d+):(\d+)/);
    if (!m) continue;
    const name = m[1] || '<top>';
    if (name === 'main') break;
    out.push(name + '@' + m[3] + ':' + m[4]);
  }
  return out.join(' < ');
}

const taken = new Set();
const thrown = new Set();
let throws = 0;

class P1 { constructor(x) { this.x = x; } }
class P2 { constructor(x) { this.y = 0; this.x = x; this.tag = true; } }

// The set reached through `this`: a body reading module-scope bindings in a
// region the pass copies is not taken (inline_guarded.cpp, isCandidate).
class Leaf {
  constructor() { this.k = 3; this.taken = taken; }
  read(p, i) {
    const v = Math.floor(p.x) + this.k;
    if (p.tag === true || i % 4999 === 0) {
this.taken.add(frames(new Error('taken').stack));
    }
    if (i % 50000 === 49999) {
throw new Error('thrown ' + i);
    }
    return v;
  }
}

class Mid {
  constructor() { this.leaf = new Leaf(); }
  step(p, i) {
    const r =
this.leaf.read(p, i);
    return r + 1;
  }
}

function drive(mid, ps, n, odd) {
  let s = 0;
  for (let i = 0; i < n; i++) {
    try {
      s +=
mid.step(i === 1 ? odd : ps[i & 7], i);
    } catch (e) {
      throws++;
      thrown.add(e.message.slice(0, 6) + ' ' + frames(e.stack));
    }
  }
  return s;
}

function report(label, s) {
  console.log(label, s, throws);
  for (const t of taken) console.log('  taken', t);
  for (const t of thrown) console.log('  thrown', t);
  taken.clear();
  thrown.clear();
  throws = 0;
}

function main() {
  const mid = new Mid();
  const ps = [];
  for (let k = 0; k < 8; k++) ps.push(new P1(k));
  report('one shape', drive(mid, ps, 300000, new P2(1)));
  const mixed = ps.map((p, k) => (k === 3 ? new P2(k) : p));
  const floor = Math.floor;
  Math.floor = (x) => floor(x);
  report('two shapes', drive(mid, mixed, 300000, new P2(1)));
}
main();
