// Error.captureStackTrace(o, fn) while fn's activation is not the speculated
// code its function object enters: a guard of that code left (the generic
// code's resume twin finishes the call), or the code retired after many exits
// (the generic code runs it). The trace still starts at fn's caller. The
// harvest of a training run saw only `{x}` receivers; a run that did not
// train meets `{y, x}` ones, at first now and then, then on every call.
const TRAINING = Math.random() < 0.5;
if (TRAINING) console.error('TRAINING');

function first(stack) {
  const m = stack.split('\n')[1] || '';
  const n = m.match(/at (?:new )?([A-Za-z_$][\w$.]*)/);
  return n ? n[1] : '<none>';
}

const seen = new Set();
function probe(p, i) {
  const v = p.x + 1;
  if (i % 100000 === 3) {
    const o = {};
    Error.captureStackTrace(o, probe);
    seen.add('probe ' + first(o.stack));
  }
  return v;
}
function caller(p, i) { return probe(p, i) * 2; }

function main() {
  const plain = [{ x: 1 }, { x: 2 }];
  let s = 0;
  for (let i = 0; i < 2000000; i++) {
    let p = plain[i & 1];
    if (!TRAINING && (i % 100000 === 3 || i > 1500000)) p = { y: 0, x: i & 7 };
    s += caller(p, i);
  }
  console.log(s);
  for (const t of seen) console.log(t);
}
main();
