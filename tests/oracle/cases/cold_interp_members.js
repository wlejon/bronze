// Interpreted cold functions (docs/type-feedback.md "Cold functions") as
// getters, setters, constructors and with default parameters, reached from
// compiled code; and a collection while interpreted frames hold objects.

class Box {
  constructor(v, scale = 2) {
    this.v = v * scale;
    this.log = [];
  }
  get doubled() { return this.v * 2; }
  set doubled(x) { this.v = x / 2; this.log.push('set ' + x); }
  static make(v) { return new Box(v); }
}

class Hot {
  constructor(n) { this.n = n; }
  get sq() { return this.n * this.n; }
}

function hotSum(o) { return o.sq + 1; }
let h = 0;
for (let i = 0; i < 20000; i++) h += hotSum(new Hot(i & 7));
console.log('hot', h);

function coldDefaults(a, b = a + 1, c = (() => a * b)()) { return [a, b, c].join(','); }
function coldArgs() { return arguments.length + ':' + Array.prototype.join.call(arguments, '-'); }

const b = Box.make(3);
console.log(b.v, b.doubled);
b.doubled = 40;
console.log(b.v, b.log.join('|'));
console.log(new Box(5, 3).v);
console.log(coldDefaults(2), coldDefaults(2, 5), coldDefaults(1, 2, 3));
console.log(coldArgs(1, 'a', null));

// Many objects made while a cold frame holds its own: they survive.
function hotAlloc(i) { return { i, s: 'x' + i, a: [i, i + 1] }; }
for (let i = 0; i < 1000; i++) hotAlloc(i);
function coldHolds() {
  const mine = [];
  for (let k = 0; k < 4; k++) mine.push({ k, tag: 'k' + k, arr: new Array(16).fill(k) });
  let junk = 0;
  for (let i = 0; i < 200000; i++) junk += hotAlloc(i).a[1] & 1;
  let ok = true;
  for (let k = 0; k < 4; k++) ok = ok && mine[k].tag === 'k' + k && mine[k].arr[15] === k;
  return ok + ' ' + junk;
}
console.log(coldHolds());

// A cold generator and async function stay compiled; called from
// interpreted code they run as ever.
function* coldGen(n) { for (let i = 0; i < n; i++) yield i * i; }
function coldUsesGen() { return [...coldGen(5)].join(','); }
console.log(coldUsesGen());
async function coldAsync(x) { return (await x) + 1; }
coldAsync(Promise.resolve(41)).then(v => console.log('async', v));
