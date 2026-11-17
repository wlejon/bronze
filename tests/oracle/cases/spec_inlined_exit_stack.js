// Error.stack taken while a guard of an inlined body has left for the
// callee's generic code (opt/inline: `resume.call @G_leaf` in top's speculated
// code, then top's own leave). The harvest of a training run saw only `{x}`
// receivers; a run that did not train meets `{tag, x}` now and then, whose
// p.x guard fails inside the copy of leaf that mid's copy inside top holds.
// The stacks taken in leaf's generic finish, and the one of an error it
// throws, name leaf, mid and top each once, as they do on every other path.
const TRAINING = Math.random() < 0.5;
if (TRAINING) console.error('TRAINING');

function frames(stack) {
  const m = stack.match(/at (?:new )?([A-Za-z_$][\w$.]*)/g) || [];
  return m.map(s => s.replace(/^at (new )?/, '')).slice(0, 4).join('>');
}

const seen = new Set();
function leaf(p) {
  const v = p.x * 2;
  if (p.tag) seen.add('taken ' + frames(new Error('t').stack));
  if (v === -2) throw new Error('boom');
  return v;
}
function mid(p) { return leaf(p) + 1; }
function top(p) { return mid(p) + 1; }

function main() {
  const plain = [{ x: 1 }, { x: 2 }, { x: 3 }, { x: 4 }];
  let s = 0;
  for (let i = 0; i < 3000000; i++) {
    let p = plain[i & 3];
    if (!TRAINING && i % 300000 === 7) p = { tag: true, x: i };
    if (!TRAINING && i % 700000 === 9) p = { tag: false, x: -1 };
    try {
      s += top(p);
    } catch (e) {
      seen.add('thrown ' + frames(e.stack));
    }
  }
  console.log(s);
  for (const t of seen) console.log(t);
}
main();
