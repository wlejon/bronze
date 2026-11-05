// The dead-zone REACH (dead_zone_reach_boundary.js) across two more statement
// forms: `let x;`, which initializes its binding to undefined and runs
// nothing, and a destructuring declaration, whose initializer, defaults and
// computed keys are code like any other. The scan walks past both when they
// can neither build a closure over the record nor name a binding still in its
// dead zone, which is what clears three.js's WebGLRenderer constructor
// (`const { canvas = createCanvasElement(), ... } = parameters; let _alpha;`)
// for the `let _gl = context;` below them. Each case pairs what must still
// throw with what may not.

function tryIt(fn) {
  try {
    return `ok:${fn()}`;
  } catch (e) {
    return e.name;
  }
}

function make() { return { made: true }; }

// The renderer's shape: a pattern with a default calling a module function, a
// bare `let`, then the binding every method reads.
function Renderer(parameters = {}) {
  const { canvas = make(), context = null, depth = true } = parameters;
  let alpha;
  if (context !== null) alpha = context.alpha;
  else alpha = false;
  let gl = context;
  if (gl === null) gl = { name: 'fallback', alpha, depth, canvas: canvas.made };
  function read() { return gl.name + ':' + gl.alpha + ':' + gl.depth + ':' + gl.canvas; }
  this.read = read;
}
console.log(new Renderer().read(), new Renderer({ context: { name: 'ctx', alpha: true } }).read());

// A default that calls a hoisted function reading a later binding: the
// mention stops the scan and the read still throws.
function hoistedDefault() {
  const { a = early() } = {};
  let later = 1;
  function early() { return later; }
  return a;
}
console.log(tryIt(hoistedDefault));

// A default naming a sibling of the same pattern bound after it.
function siblingDefault() {
  const { a = b, b } = { b: 1 };
  return a + b;
}
console.log(tryIt(siblingDefault));

// An initializer naming a binding declared below.
function laterInit() {
  const { p } = { p: q };
  let q = 1;
  return p;
}
console.log(tryIt(laterInit));

// A default that builds a closure stops the scan: the closure can read `z`.
function closureDefault() {
  const { f = () => z } = {};
  const seen = tryIt(f);
  let z = 2;
  return seen + '/' + f();
}
console.log(closureDefault());

// A getter on the destructured object runs user code that cannot see this
// record; the bindings after it are still read correctly by closures.
function getterSource() {
  const src = { get v() { return 41; } };
  const { v } = src;
  let after = v + 1;
  const read = () => after;
  return read();
}
console.log(getterSource());

// `let x;` is undefined, not a dead zone, from its declaration on.
function bareLet() {
  let x;
  const before = typeof x;
  const read = () => x;
  x = 3;
  return before + ':' + read();
}
console.log(bareLet());

// ... and still a dead zone above it, for a hoisted function called first.
function bareLetCalledEarly() {
  const r = tryIt(() => peek());
  let x;
  function peek() { return x; }
  return r + '/' + peek();
}
console.log(bareLetCalledEarly());

// `var v;` runs nothing and ends no dead zone: a hoisted function called
// below it still reads `later` in its dead zone.
function varDeclaresNothing() {
  var v;
  let out;
  try { out = hf(); } catch (e) { out = e.name; }
  let later = 1;
  function hf() { return later; }
  return out + '/' + hf() + '/' + v;
}
console.log(varDeclaresNothing());

// Array patterns, holes and rest: every bound name is initialized past it.
function arrayPattern() {
  const [a, , b = make().made, ...rest] = [1, 2, undefined, 4, 5];
  let tail = rest.length;
  const read = () => a + ':' + b + ':' + tail;
  return read();
}
console.log(arrayPattern());

// Module level: a pattern and a bare `let` ahead of the functions that read them.
const { modA = make().made, modB } = { modB: 'b' };
let modC;
function readModule() { return modA + ':' + modB + ':' + modC; }
modC = 'c';
console.log(readModule());
