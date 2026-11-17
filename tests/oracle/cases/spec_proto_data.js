// Reads of data properties found on the prototype chain (data member cells,
// opt/speculate/member_data.cpp): class constants on a prototype read from
// instances and Object.create receivers, a default some instances override
// as own (a site of own and proto ways), a key two links up, a key absent on
// some receivers, and an object-valued constant only its prototype keeps
// alive. Trained on the stable arm (run.py's switch mode), then the chains
// change under the speculated reads in every way that changes the answer: a
// value written, an own property shadowing, a delete, a prototype swapped, a
// getter defined over the property, the middle link gaining the key, a
// prototype in dictionary mode, frozen, and given a string where a Number
// was. Every read keeps its right answer throughout.
const TRAINING = Math.random() < 0.5;
class GL { constructor() { this.id = 1; } }
GL.prototype.WRAP_S = 10242;
GL.prototype.WRAP_T = 10243;
GL.prototype.CLAMP = 33071;
class Opt { constructor(n) { this.n = n; if (n % 3 === 0) this.scale = n; } }
Opt.prototype.scale = 2;
class Base { constructor() { this.b = 0; } }
Base.prototype.DEEP = 7;
class Mid extends Base { constructor() { super(); this.m = 1; } }
class Leaf extends Mid { constructor() { super(); this.l = 2; } }

function consts(g) { return g.WRAP_S + g.WRAP_T - g.CLAMP; }
function scaleOf(o) { return o.scale; }
function deep(o) { return o.DEEP; }
function maybe(o) { const v = o.CLAMP; return v === undefined ? -1 : v; }
function cfgLen(o) { return o.cfg.items.length + o.cfg.k; }
function loopRead(g, n) {
  let s = 0;
  for (let i = 0; i < n; i++) { const v = g.WRAP_S; s += typeof v === 'number' ? v : v.length * 1000; }
  return s;
}
function loopWrite(g, n) {
  let s = 0;
  for (let i = 0; i < n; i++) { s += g.WRAP_T; GL.prototype.WRAP_T = i & 3; }
  return s;
}

function makeHolder() {
  // A prototype made here and kept alive only through its instances' chain;
  // its constant is an object nothing else names.
  const proto = { cfg: { items: [1, 2, 3], k: 10 } };
  const objs = [];
  for (let i = 0; i < 4; i++) { const o = Object.create(proto); o.i = i; objs.push(o); }
  return objs;
}
function churn(n) {
  let keep = null;
  for (let i = 0; i < n; i++) keep = { a: i, b: [i, i + 1], c: 'x' + i };
  return keep.a;
}

const gls = [new GL(), Object.create(GL.prototype), new GL(), Object.create(GL.prototype)];
const opts = [];
for (let i = 0; i < 12; i++) opts.push(new Opt(i));
const deeps = [new Mid(), new Leaf(), new Mid(), new Leaf()];
const mixed = [new GL(), { CLAMP: 5 }, Object.create(null), new Opt(1)];
let holders = makeHolder();

function round(tag) {
  // Each read's distinct answers over the round, so a wrong one shows.
  const seen = [new Set(), new Set(), new Set(), new Set(), new Set()];
  for (let i = 0; i < 4000; i++) {
    seen[0].add(consts(gls[i & 3]));
    seen[1].add(scaleOf(opts[i % 12]));
    seen[2].add(deep(deeps[i & 3]));
    seen[3].add(maybe(mixed[i & 3]));
    seen[4].add(cfgLen(holders[i & 3]));
  }
  const loops = loopRead(gls[0], 1000) + '|' + loopRead(gls[1], 1000);
  console.log(tag, seen.map((s) => [...s].map(String).join('/')).join(' '), loops);
}

round('warm');
churn(20000);
round('after gc');
for (let r = 0; r < 3; r++) console.log('loop', loopWrite(gls[0], 1000), GL.prototype.WRAP_T);
GL.prototype.WRAP_T = 10243;
if (TRAINING) {
  console.error('TRAINING');
} else {
  GL.prototype.WRAP_S = 1;                       // a value written
  round('written');
  gls[2].WRAP_T = 100;                           // an own property shadows
  gls[3].CLAMP = 200;
  round('shadowed');
  delete Base.prototype.DEEP;                    // deleted: absent now
  round('deleted');
  Base.prototype.DEEP = 8;
  Mid.prototype.DEEP = 9;                        // the middle link gains the key
  round('middle');
  Object.setPrototypeOf(gls[1], { WRAP_S: 3, WRAP_T: 4, CLAMP: 5 });  // a receiver's prototype swapped
  round('swapped');
  Object.setPrototypeOf(Opt.prototype, { scale: 6 });                // a holder's own prototype swapped
  delete Opt.prototype.scale;
  round('holder swapped');
  let reads = 0;
  Object.defineProperty(GL.prototype, 'CLAMP', { get() { reads++; return 11; }, configurable: true });
  round('getter');
  console.log('getter reads', reads > 0);
  Object.defineProperty(GL.prototype, 'CLAMP', { value: 12, writable: true, configurable: true });
  for (let k = 0; k < 40; k++) { GL.prototype['tmp' + k] = k; delete GL.prototype['tmp' + k]; }
  round('dictionary');
  GL.prototype.WRAP_S = 'S';                     // a string where a Number was
  round('string');
  Object.freeze(GL.prototype);
  round('frozen');
  holders = makeHolder();                        // a fresh, young holder
  round('young');
  churn(50000);
  round('moved');
}
round('end');
