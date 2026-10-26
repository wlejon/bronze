// A small function called from a function that holds a loop is a candidate
// for the lowering's inliner (lower/inline_small.cpp). One that builds an
// Error, or calls anything that could, must keep its own frame: the stack
// names it and then its caller, as it does from a caller with no loop.

function getFrames(stack) {
  const m = stack.match(/at (\S+)/g);
  return m ? m.map(s => s.replace('at ', '')) : [];
}

function thrower() { return new Error('x').stack; }
function viaCall() { return thrower(); }
function traced() { return getFrames(new Error('y').stack)[0]; }

function caller() {
  let s = 0;
  for (let i = 0; i < 3; i++) s += i;
  const a = getFrames(thrower());
  console.log(a[0], a[1]);
  const b = getFrames(viaCall());
  console.log(b[0], b[1], b[2]);
  console.log(traced());
  return s;
}
caller();

// The script itself holds a loop, so a call from its top level is one too.
for (let k = 0; k < 2; k++) {}
const t = getFrames(thrower());
// Its caller's frame is still there below it.
console.log(t[0], t.length > 1);
