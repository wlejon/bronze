// The keyed shape of a receiver that is no plain object (il2mir
// PropertyLoweringHelper::keyed_shape): the object test and the header test
// branch to a cold block that hands every guard after them a stand-in shape,
// a readable block whose family stamp is UNSTAMPED and whose double-slot map
// is empty. No guard tests plainness again: a family range, an identity cell,
// a cache way and a double-slot store term all compare against it and must
// refuse it, so each line below answers what the ordinary [[Get]] / [[Set]]
// answers.
//
// Every method is warmed on plain instances first (cells published, families
// stamped, ways filled), then reached with a `this` that is an Array, a
// function, a Map, a boxed or bare primitive, a Proxy, a typed array, a
// null-prototype object and a frozen instance, more than once in one body so
// that the memo carries the stand-in from one guard to the next.

const N = 3000;

class Base {
  constructor() {
    this.a = 1;
    this.b = 2;
    this.d = 0.5; // a double slot: a store term reads the double-slot map
  }
  // family-guarded reads of `this`, twice each
  read() {
    return this.a * 100 + this.b + this.a;
  }
  // stores: a boxed slot and a double slot, then reads back
  write(v) {
    this.a = v;
    this.d = v + 0.25;
    return this.a + ':' + this.d + ':' + this.b;
  }
  // an inlined call between the reads (the memo crosses its guard)
  sum() {
    return this.read() + this.b + this.read();
  }
}
class Sub extends Base {
  constructor() {
    super();
    this.c = 3;
  }
  readC() {
    return this.c + this.a;
  }
}

function fmt(v) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (typeof v === 'number' && Number.isNaN(v)) return 'NaN';
  return String(v);
}

function run(label, fn) {
  let out;
  try {
    out = fmt(fn());
  } catch (e) {
    out = 'throws ' + e.constructor.name;
  }
  console.log(label + ': ' + out);
}

// Warm every site on plain instances.
let acc = 0;
for (let i = 0; i < N; i++) {
  const o = i % 3 === 0 ? new Base() : new Sub();
  acc += o.read() + o.sum();
  o.write(i);
  if (o instanceof Sub) acc += o.readC();
}
console.log('warm', acc);

const read = Base.prototype.read;
const write = Base.prototype.write;
const sum = Base.prototype.sum;
const readC = Sub.prototype.readC;

function fnRecv() {}
fnRecv.a = 4;
fnRecv.b = 5;
fnRecv.c = 6;

const arr = [7, 8];
arr.a = 9;
arr.b = 10;
arr.c = 11;

const map = new Map();
map.a = 12;
map.b = 13;

const nullProto = Object.create(null);
nullProto.a = 14;
nullProto.b = 15;
nullProto.c = 16;

const proxy = new Proxy({ a: 17, b: 18, c: 19 }, {
  get(t, k) {
    return k in t ? t[k] * 2 : 'none';
  },
});

const frozen = Object.freeze(new Sub());

const ta = new Float64Array(2);
ta.a = 20;
ta.b = 21;

const receivers = [
  ['array', arr],
  ['function', fnRecv],
  ['map', map],
  ['nullProto', nullProto],
  ['proxy', proxy],
  ['frozen', frozen],
  ['typedArray', ta],
  ['number', 42],
  ['string', 'str'],
  ['boolean', true],
  ['boxedNumber', new Number(3)],
];

for (let round = 0; round < 2; round++) {
  for (const [name, r] of receivers) {
    run(name + '.read', () => read.call(r));
    run(name + '.sum', () => sum.call(r));
    run(name + '.readC', () => readC.call(r));
  }
}
// Stores go last: they change the receivers.
for (const [name, r] of receivers) {
  run(name + '.write', () => write.call(r, 2));
  run(name + '.read after write', () => read.call(r));
}

// A receiver that changes kind between calls at one site: plain, then not,
// then plain again, in one loop, so the site's guard sees both answers.
const mixed = [new Sub(), arr, new Base(), fnRecv, new Sub(), 7, new Base()];
let line = [];
for (let i = 0; i < 3 * mixed.length; i++) {
  const r = mixed[i % mixed.length];
  let v;
  try {
    v = read.call(r);
  } catch (e) {
    v = 'T';
  }
  line.push(fmt(v));
}
console.log('mixed', line.join(','));

// A plain instance whose shape moves after warming: a deleted field and a
// redefined accessor, read through the same family sites.
const moved = new Sub();
delete moved.b;
run('deleted.read', () => moved.read());
run('deleted.sum', () => moved.sum());
const acc2 = new Sub();
Object.defineProperty(acc2, 'a', { get() { return 1000; } });
run('accessor.read', () => acc2.read());
run('accessor.readC', () => acc2.readC());
run('accessor.write', () => acc2.write(5));
