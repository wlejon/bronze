// Many distinct first properties added to fresh objects (one long transition
// list on the empty-object root), then the same names again with other
// attributes and value kinds: every object must land on the layout it built.
'use strict';
const N = 3000;
let sum = 0;
for (let i = 0; i < N; i++) {
  const o = {};
  o['k' + i] = i;
  o.after = i * 2;
  sum += o['k' + i] + o.after + Object.keys(o).length;
}
console.log('fresh', sum);

const made = [];
for (let i = 0; i < 40; i++) {
  const name = 'k' + i;
  const a = {}; a[name] = i;                         // number
  const b = {}; b[name] = 'v' + i;                   // string
  const c = {}; Object.defineProperty(c, name, { value: i, enumerable: false, writable: true, configurable: true });
  const d = {}; Object.defineProperty(d, name, { get() { return 'g' + i; }, enumerable: true, configurable: true });
  const e = {}; Object.defineProperty(e, name, { value: i, enumerable: true, writable: false, configurable: false });
  made.push([name, a, b, c, d, e]);
}
for (const [name, a, b, c, d, e] of made.slice(0, 40)) {
  const line = [a[name], b[name], c[name], d[name], e[name],
    Object.keys(a).join(), Object.keys(c).join(), Object.keys(d).join(),
    JSON.stringify(Object.getOwnPropertyDescriptor(e, name)),
    typeof Object.getOwnPropertyDescriptor(d, name).get];
  if (name === 'k0' || name === 'k17' || name === 'k39') console.log(name, line.join(' | '));
}

let ok = true;
for (let i = 0; i < N; i += 7) {
  const name = 'k' + i;
  const x = {};
  x[name] = (i & 1) ? 'odd' : i + 0.5;
  x.tail = true;
  const expect = (i & 1) ? 'odd' : i + 0.5;
  if (x[name] !== expect || x.tail !== true || Object.keys(x).join() !== name + ',tail') ok = false;
}
console.log('reuse', ok);

const sym = [];
for (let i = 0; i < 30; i++) sym.push(Symbol('s' + i));
let symOk = true;
for (let r = 0; r < 2; r++) {
  for (let i = 0; i < 30; i++) {
    const o = {};
    o[sym[i]] = i;
    o['s' + i] = -i;
    if (o[sym[i]] !== i || o['s' + i] !== -i || Object.getOwnPropertySymbols(o)[0] !== sym[i]) symOk = false;
  }
}
console.log('symbols', symOk, String(sym[3]));
