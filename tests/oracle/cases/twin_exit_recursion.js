// Guards failing in a recursive function, at every depth and on the way
// back up: each activation that leaves finishes in the resume twin, whose
// recursive calls enter the speculated code again; a recursion that runs out
// of stack through both still throws a RangeError the caller catches.

class N {
  constructor(d) { this.v = d; this.kid = d > 0 ? new N(d - 1) : null; }
}

function depth(n, odd, at) {
  if (n.kid === null) return n.v;
  const k = at === n.v ? odd : n.kid;
  const below = depth(n.kid, odd, at);
  return below + k.v + 1;        // the read after the call meets `odd`
}

function forever(n, o) {
  return forever(n, o) + o.v;
}

const root = new N(60);
const odd = { w: 0, v: 1000 };
let s = 0;
for (let r = 0; r < 4000; r++) s += depth(root, odd, r % 90);
console.log(s);
console.log(depth(root, { v: 'x' }, 30));

let caught = 0;
for (let i = 0; i < 3; i++) {
  try { forever(root, i === 1 ? odd : root); } catch (e) { if (e instanceof RangeError) caught++; }
}
console.log(caught);
