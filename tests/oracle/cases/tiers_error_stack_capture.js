// Error.stack is captured when the Error is made: the walk that captures it
// reuses the unwind rows of earlier walks while no code has come or gone
// (brass native_unwind.cpp), and the thread's stack bounds are asked once.
// Each stack is checked by its frames' names and positions, which do not
// depend on where the file lives, up to the script's top level (what is
// below it is the host's: node's module loader, nothing in bronze).

function frames(stack) {
  const out = [];
  for (const line of stack.split('\n').slice(1)) {
    const m = line.match(/at (?:(\S+(?: \S+)?) \()?.*?([^\/\\]+\.js):(\d+):(\d+)/);
    if (m && m[1] && m[1] !== 'Object.<anonymous>') out.push(m[1] + '@' + m[3] + ':' + m[4]);
    else if (m) { out.push('<top>@' + m[3] + ':' + m[4]); break; }
    else out.push(line.trim());
  }
  return out.join(' < ');
}

function make(msg) { return new Error(msg); }
function viaHelper(msg) { return make(msg); }
function reader(e) { return frames(e.stack); }

// Made in one function, read in another after its frames are gone.
const early = viaHelper('early');
console.log('gone', reader(early));

// Read twice: the same text both times.
console.log('twice', early.stack === early.stack, early.stack.split('\n')[0]);

// The limit in force when the Error is made decides its frame count; a
// later change does not reach a stack already taken.
function deep(n) { return n === 0 ? new Error('deep') : deep(n - 1); }
const saved = Error.stackTraceLimit;
Error.stackTraceLimit = 3;
const three = deep(8);
Error.stackTraceLimit = 1;
const one = deep(8);
Error.stackTraceLimit = 0;
const none = deep(8);
Error.stackTraceLimit = saved;
console.log('limit3', frames(three.stack));
console.log('limit1', frames(one.stack));
console.log('limit0', JSON.stringify(none.stack));
console.log('default', frames(deep(3).stack));

// Through a builtin's callback.
const fromMap = [1].map(function mapper(x) { return new Error('map ' + x); })[0];
console.log('map', frames(fromMap.stack));

// Hot: the same frames every time, inlined callees included, while the
// functions tier up around them.
class Leaf {
  make(i) { return new Error('leaf ' + i); }
}
class Branch {
  constructor() { this.leaf = new Leaf(); }
  grow(i) { return this.leaf.make(i); }
}
function hot(b, i) { return b.grow(i); }
const b = new Branch();
const seen = new Map();
for (let i = 0; i < 30000; i++) {
  const e = (i & 3) === 0 ? hot(b, i) : viaHelper('loop');
  const f = frames(e.stack);
  seen.set(f, (seen.get(f) || 0) + 1);
}
for (const [f, n] of seen) console.log('hot', n, f);

// captureStackTrace on a plain object.
const plain = {};
function capturer() { Error.captureStackTrace(plain); }
capturer();
console.log('capture', frames(plain.stack));
