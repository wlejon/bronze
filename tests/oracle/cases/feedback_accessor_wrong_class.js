// A profile names a receiver's prototype by its constructor's `name`, and a
// build reading it maps that name to the classes declared under it
// (docs/type-feedback.md, "F, accessor targets"). Here the name is shared: the
// hot receivers are made by a constructor FUNCTION `Thing`, whose prototype
// has accessors of its own, while a CLASS `Thing` elsewhere declares others.
// The build's guess is the class's getter and setter, wrong for every hot
// receiver: the accessor cell must refuse to publish them, and every line
// read as the ordinary [[Get]] / [[Set]].

function Thing(v) {
  this._v = v;
}
Object.defineProperty(Thing.prototype, 'v', {
  get() { return this._v * 2; },
  set(x) { this._v = x + 100; },
  configurable: true,
});

function makeClassThing(v) {
  class Thing {
    constructor(v) { this._v = v; }
    get v() { return this._v - 1; }
    set v(x) { this._v = -x; }
  }
  return new Thing(v);
}

function read(o) { return o.v; }
function write(o, x) { o.v = x; }

function run(list) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    write(list[i], read(list[i]) + 1);
    s += read(list[i]);
  }
  return s;
}

const things = [];
for (let i = 0; i < 32; i++) things.push(new Thing(i));
let total = 0;
for (let r = 0; r < 100; r++) total += run(things);
console.log('function Thing', total, things[5]._v, read(things[5]));

const classy = makeClassThing(10);
console.log('class Thing', read(classy), run([classy]), classy._v, classy.constructor.name);
console.log('mixed', run([things[0], classy, things[1]]), things[0]._v, classy._v);

// The function's prototype accessor replaced by the class's own getter: now
// the guess is right for the function's instances too, and the cell may
// publish it at the new epoch.
const classGetter = Object.getOwnPropertyDescriptor(Object.getPrototypeOf(classy), 'v');
Object.defineProperty(Thing.prototype, 'v', classGetter);
console.log('borrowed', run(things), things[5]._v, read(things[5]));
