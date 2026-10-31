// oracle: Math.random drawn, no drawn value printed
// Math.random() on the pristine builtin compiles to the generator called
// directly. Its values are never printed (they are random); what is printed is
// what must hold of them: in [0, 1), a fresh draw every call (never merged,
// hoisted out of a loop or dropped), and the same through every way of calling.

function inUnit(x) {
  return typeof x === 'number' && x >= 0 && x < 1 && !Object.is(x, -0);
}

// Draws in a loop are all distinct: a hoisted or merged call would repeat.
function distinctInLoop(n) {
  const seen = new Set();
  let ok = true;
  for (let i = 0; i < n; i++) {
    const x = Math.random();
    if (!inUnit(x)) ok = false;
    seen.add(x);
  }
  return [ok, seen.size === n];
}
console.log('loop', distinctInLoop(2000));

// Two calls in one expression are two draws.
function pair() {
  let same = 0;
  for (let i = 0; i < 500; i++) if (Math.random() === Math.random()) same++;
  return same;
}
console.log('pair same', pair());

// A draw whose value is unused still runs; the next one is fresh.
function unused() {
  const xs = [];
  for (let i = 0; i < 100; i++) {
    Math.random();
    xs.push(Math.random());
  }
  return new Set(xs).size;
}
console.log('unused', unused());

// The integer idioms three.js uses, in range.
function ints() {
  let ok = true;
  for (let i = 0; i < 1000; i++) {
    const d = Math.random() * 0xffffffff | 0;
    if ((d | 0) !== d) ok = false;
    const b = Math.floor(Math.random() * 6);
    if (b < 0 || b > 5) ok = false;
  }
  return ok;
}
console.log('ints', ints());

// Every other way of calling it: alias, computed key, call/apply, optional,
// with ignored arguments that are still evaluated.
{
  const r = Math.random;
  let evaluated = 0;
  const vals = [
    r(),
    Math['random'](),
    Math.random.call(null),
    Math.random.apply(undefined, []),
    Math.random?.(),
    Math?.random(),
    Math.random(evaluated++, evaluated++),
  ];
  console.log('forms', vals.every(inUnit), new Set(vals).size, evaluated);
  console.log('meta', typeof Math.random, Math.random.length, Math.random.name);
}

// Under try/finally, in a generator, in a closure, as a default parameter.
{
  function tf() {
    try {
      return Math.random();
    } finally {
      Math.random();
    }
  }
  function* gen(n) {
    for (let i = 0; i < n; i++) yield Math.random();
  }
  const mk = () => () => Math.random();
  function dflt(x = Math.random()) { return x; }
  const g = [...gen(50)];
  const f = mk();
  console.log('contexts', inUnit(tf()), g.every(inUnit), new Set(g).size, inUnit(f()), inUnit(dflt()), dflt(7));
}

// Hot enough to tier up, mixing the result into arithmetic and strings.
{
  let sum = 0;
  let strOk = true;
  for (let i = 0; i < 20000; i++) {
    const x = Math.random();
    sum += x;
    if (('' + x).length === 0) strOk = false;
  }
  const mean = sum / 20000;
  console.log('mean', mean > 0.45 && mean < 0.55, strOk);
}
