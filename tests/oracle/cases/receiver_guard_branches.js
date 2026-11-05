// Receiver guards that branch: the object test and the plain-header test of a
// guard whose receiver the shape memo does not hold go straight to the site's
// miss (il_static_slot.cpp plain_shape_or_miss), at a lone static site, a run
// of static sites on one receiver, a direct method call, and the way-0 inline
// read and write of a monomorphic site. Each kind of site is warmed on the
// receivers its claim names, then handed every other kind of value: numbers,
// strings, booleans, null and undefined, symbols, BigInts, arrays, typed
// arrays, functions, proxies, maps, dates, frozen and reshaped objects, and
// a subclass. Every line must read as the ordinary [[Get]] / [[Set]] / call.

const N = 2000;

class Vec {
  constructor(x, y) {
    this.x = x;
    this.y = y;
    this.z = 0;
    this.w = 1;
    this.tag = 'v';
  }
  // A run on `this`: reads and writes of five fields, one past the inline slots.
  bump() {
    this.x = this.x + 1;
    this.y = this.y + this.w;
    this.tag = this.tag + '';
    return this.x * 10 + this.y;
  }
  len() {
    return this.x + this.y + this.z;
  }
}

class Vec3 extends Vec {
  constructor(x, y) {
    super(x, y);
    this.extra = 7;
  }
  len() {
    return 1000 + this.x;
  }
}

// Lone static sites, a read and a write.
function readX(v) {
  return v.x;
}
function writeY(v, k) {
  v.y = k;
  return v.y;
}
// A direct method call: inference names Vec.len.
function callLen(v) {
  return v.len();
}
// Two sites on one receiver around a call: the second guard after the call
// meets the receiver fresh.
function readAround(v, f) {
  const a = v.x;
  f();
  return a + v.z;
}
// Monomorphic literal sites (no class layout): way-0 inline read and write.
function litRead(o) {
  return o.k;
}
function litWrite(o, k) {
  o.k = k;
  return o.k;
}

function show(f) {
  try {
    const r = f();
    return typeof r === 'symbol' ? r.toString() : typeof r === 'bigint' ? r + 'n' : String(r);
  } catch (e) {
    return e.constructor.name;
  }
}

let warm = 0;
for (let i = 0; i < N; i = i + 1) {
  const v = new Vec(i, 2);
  warm = warm + readX(v) + writeY(v, i) + callLen(v) + v.bump() + readAround(v, () => 0);
  warm = warm + litRead({ k: i }) + litWrite({ k: 1 }, i);
}
console.log('warm', warm);

const sym = Symbol('s');
const fn = function () {};
fn.x = 'fnx';
fn.k = 'fnk';
fn.len = () => 'fnlen';
const arr = [1, 2, 3];
arr.x = 'arrx';
arr.k = 'arrk';
const ta = new Float64Array(4);
ta.x = 'tax';
const frozen = Object.freeze(new Vec(5, 6));
const reshaped = new Vec(8, 9);
delete reshaped.x;
reshaped.x = 'late';
const dict = new Vec(1, 1);
for (let i = 0; i < 40; i = i + 1) dict['p' + i] = i;
for (let i = 0; i < 40; i = i + 2) delete dict['p' + i];
const proxy = new Proxy(new Vec(3, 4), {
  get(t, k) {
    return k === 'len' ? () => 'proxied' : typeof t[k] === 'number' ? t[k] * 100 : t[k];
  },
});
const map = new Map();
map.x = 'mapx';
const date = new Date(0);
date.k = 'datek';
const sub = new Vec3(11, 12);
const protoOnly = Object.create(new Vec(21, 22));
const lit = { x: 'litx', y: 0, z: 1, len() { return 'litlen'; }, k: 'litk' };

const recvs = [
  ['int', 7], ['double', 2.5], ['nan', NaN], ['str', 'abc'], ['true', true], ['null', null],
  ['undef', undefined], ['sym', sym], ['big', 12n], ['fn', fn], ['arr', arr], ['ta', ta],
  ['frozen', frozen], ['reshaped', reshaped], ['dict', dict], ['proxy', proxy], ['map', map],
  ['date', date], ['sub', sub], ['protoOnly', protoOnly], ['lit', lit],
];

for (const [name, r] of recvs) {
  const line = [
    name,
    show(() => readX(r)),
    show(() => writeY(r, 'w')),
    show(() => callLen(r)),
    show(() => readAround(r, () => 0)),
    show(() => litRead(r)),
    show(() => litWrite(r, 'lk')),
  ];
  if (r instanceof Vec && !Object.isFrozen(r)) line.push(show(() => r.bump()));
  console.log(line.join(' '));
}

// A shape change between two reads of one receiver around a call.
const moving = new Vec(1, 2);
console.log('around', readAround(moving, () => { delete moving.z; moving.z = 40; }));
console.log('around2', readAround(moving, () => { Object.setPrototypeOf(moving, null); }));
// The warmed sites still answer plain receivers after all of the above.
let after = 0;
for (let i = 0; i < 100; i = i + 1) {
  const v = new Vec(i, 3);
  after = after + readX(v) + writeY(v, 1) + callLen(v) + v.bump() + litRead({ k: 2 });
}
console.log('after', after);
