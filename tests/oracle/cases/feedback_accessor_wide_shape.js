// A profile lists at most 32 of a shape's keys (runtime/type_feedback.cpp):
// past that the name ends `,+N` and does not say whether the key is one of
// the N. A build reading it still guesses the prototype's accessor for a key
// the listed ones lack (docs/type-feedback.md, "F, accessor targets"), and the
// accessor cell decides. Here the hot receivers of one site have the key as
// an OWN data property past the 32nd, defined over the class's accessor: the
// guess is wrong for every one of them, and each read and write must be the
// ordinary [[Get]] / [[Set]] of the own property. A second site meets
// receivers of the same wide class without the own key, where the guess is
// right, and then ones whose accessor a subclass overrides.

class Wide {
  constructor(i, own) {
    for (let k = 0; k < 40; k++) this['f' + k] = i + k;
    this._v = i;
    this.count = 0;
    if (own) Object.defineProperty(this, 'v', { value: -i, writable: true, enumerable: true, configurable: true });
  }
  get v() { this.count++; return this._v * 2; }
  set v(x) { this.count++; this._v = x + 100; }
}

class Narrower extends Wide {
  get v() { return 'sub ' + this._v; }
  set v(x) { this._v = x - 1; }
}

// Another declarer of the key, so that no guess of lowering's names one class
// for these sites: only the profile does.
class Other {
  get v() { return 'other'; }
  set v(x) { this.seen = x; }
}

function readOwn(o) { return o.v; }
function writeOwn(o, x) { o.v = x; }
function readAcc(o) { return o.v; }
function writeAcc(o, x) { o.v = x; }

const owners = [];
const plain = [];
for (let i = 0; i < 50; i++) {
  owners.push(new Wide(i, true));
  plain.push(new Wide(i, false));
}

let a = 0, b = 0;
for (let r = 0; r < 40; r++) {
  for (const o of owners) { writeOwn(o, readOwn(o) + 1); a += readOwn(o); }
  for (const o of plain) { writeAcc(o, readAcc(o) % 1000); b += readAcc(o); }
}
console.log('own', a, owners[3].v, owners[3].count, owners[3]._v, Object.keys(owners[3]).length);
console.log('accessor', b, plain[3].v, plain[3].count, plain[3]._v);

// The guess meets what it did not see: a subclass's override, an own key on
// the accessor site, a frozen own key, a primitive and a plain object.
const odd = [new Other(), new Narrower(7, false), new Wide(8, true), Object.freeze(new Wide(9, true)), 'str', { v: 5 }];
for (const o of odd) {
  try {
    writeAcc(o, 3);
    console.log('odd', typeof o, readAcc(o));
  } catch (e) {
    console.log('odd threw', e instanceof TypeError, readAcc(o));
  }
}
// The accessor removed from the class: every site reads the own keys or nothing.
delete Wide.prototype.v;
console.log('deleted', readAcc(plain[1]), readOwn(owners[1]), plain[1].count);
writeAcc(plain[1], 77);
console.log('after', readAcc(plain[1]), plain[1]._v, Object.keys(plain[1]).includes('v'));
