// Own-slot callbacks inlined behind a test of the closure's code
// (lower/inline_guarded.cpp, "own callees"). `this._onChangeCallback()` in
// Rot.set finds, on every Rot a Holder owns, that Holder's own closure of
// `onRot`: one function, a different closure and environment per receiver,
// guessed from the program's `_onChange(cb)` stores. The inlined copy of
// `onRot` must run with the environment of the closure the receiver holds,
// and everything else in the slot (another closure swapped in, a closure
// replaced, deleted or turned into a getter while the loop runs, a number, a
// bound function, a Proxy, nothing at all) must do what the call would.

class Quat {
  constructor() { this.v = 0; this.calls = 0; }
  setFrom(r) { this.v = r._x * 2; this.calls++; return this; }
}

class Rot {
  constructor() { this._x = 0; }
  set(x) { this._x = x; this._onChangeCallback(); return this; }
  get x() { return this._x; }
  set x(v) { this._x = v; this._onChangeCallback(); }
  _onChange(cb) { this._onChangeCallback = cb; return this; }
  _onChangeCallback() {}
}

const log = [];
let replacement = null;

class Holder {
  constructor(tag) {
    const rot = new Rot();
    const quat = new Quat();
    let count = 0;
    let mode = 'plain';
    let other = null;
    function onRot() {
      if (mode === 'throw') throw new Error('thrown by ' + tag);
      if (mode === 'replace') { mode = 'plain'; rot._onChangeCallback = replacement; }
      if (mode === 'reshape') { mode = 'plain'; rot.extra = tag; }
      if (mode === 'delete') { mode = 'plain'; delete rot._onChangeCallback; }
      if (mode === 'reenter') { mode = 'plain'; other.rot.set(rot._x + 100); }
      quat.setFrom(rot);
      count++;
    }
    rot._onChange(onRot);
    this.rot = rot;
    this.quat = quat;
    this.onRot = onRot;
    this.count = () => count;
    this.setMode = (m) => { mode = m; };
    this.setOther = (o) => { other = o; };
  }
}

// The second closure Rot's callback is guessed to be: an arrow, whose `this`
// is the Watcher it was made in, not the Rot it is called on.
class Watcher {
  constructor() {
    this.rot = new Rot();
    this.hits = 0;
    this.rot._onChange(() => { this.hits += this.rot._x; });
  }
}

// A function expression whose `this` is the receiver of the call.
class Spin {
  constructor() { this.a = 0; this.seen = 0; }
  turn(a) { this.a = a; this._onChangeCallback(); return this; }
  _onChange(cb) { this._onChangeCallback = cb; return this; }
  _onChangeCallback() {}
}
function makeSpin() {
  const s = new Spin();
  s._onChange(function () { this.seen += this.a; });
  return s;
}

function turnAll(s, n) { for (let i = 1; i <= n; i++) s.turn(i); return s; }
function spin(r, from, n) { for (let i = 0; i < n; i++) r.set(from + i); return r; }
function spinX(r, from, n) { for (let i = 0; i < n; i++) r.x = from + i; return r; }

const h1 = new Holder('h1');
const h2 = new Holder('h2');
h1.setOther(h2);
h2.setOther(h1);

spin(h1.rot, 0, 100);
spin(h2.rot, 1000, 50);
console.log('envs', h1.quat.v, h1.quat.calls, h1.count(), h2.quat.v, h2.quat.calls, h2.count());

spinX(h1.rot, 10, 5);
console.log('setter', h1.quat.v, h1.quat.calls, h1.count());

// Swapped: h1's Rot holds h2's closure, whose environment is h2's.
h1.rot._onChange(h2.onRot);
h2.rot._onChange(h1.onRot);
spin(h1.rot, 7, 3);
console.log('swap', h1.quat.v, h1.quat.calls, h2.quat.v, h2.quat.calls, h2.count());
h1.rot._onChange(h1.onRot);
h2.rot._onChange(h2.onRot);

const w = new Watcher();
const w2 = new Watcher();
spin(w.rot, 1, 4);
spin(w2.rot, 10, 2);
console.log('arrow', w.hits, w2.hits);

const s = turnAll(makeSpin(), 3);
console.log('this', s.seen);

h1.setMode('throw');
try {
  spin(h1.rot, 0, 10);
} catch (e) {
  console.log('throw', e.message, h1.rot._x, h1.count());
}
h1.setMode('plain');

replacement = function () { log.push('r' + this._x); };
h1.setMode('replace');
spin(h1.rot, 20, 3);
console.log('replace', h1.quat.v, h1.quat.calls, h1.count(), log.join(','));
log.length = 0;
h1.rot._onChange(h1.onRot);

h1.setMode('reshape');
spin(h1.rot, 30, 3);
console.log('reshape', h1.quat.v, h1.quat.calls, h1.count(), h1.rot.extra);

h1.setMode('delete');
spin(h1.rot, 40, 3);
console.log('delete', h1.quat.v, h1.quat.calls, h1.count(), Object.prototype.hasOwnProperty.call(h1.rot, '_onChangeCallback'));
h1.rot._onChange(h1.onRot);

h1.setMode('reenter');
spin(h1.rot, 50, 1);
console.log('reenter', h1.quat.v, h1.quat.calls, h2.quat.v, h2.quat.calls);

let gets = 0;
Object.defineProperty(h1.rot, '_onChangeCallback', {
  get() { gets++; return h1.onRot; },
  configurable: true,
});
spin(h1.rot, 60, 3);
console.log('getter', gets, h1.quat.v, h1.quat.calls, h1.count());
delete h1.rot._onChangeCallback;
h1.rot._onChange(h1.onRot);

h1.rot._onChangeCallback = 42;
try {
  spin(h1.rot, 70, 2);
} catch (e) {
  console.log('nonfn', e instanceof TypeError, h1.rot._x);
}

h1.rot._onChangeCallback = h1.onRot.bind(null);
spin(h1.rot, 80, 2);
console.log('bound', h1.quat.v, h1.quat.calls);

h1.rot._onChangeCallback = new Proxy(h1.onRot, {
  apply(t, th, args) { log.push('p'); return Reflect.apply(t, th, args); },
});
spin(h1.rot, 90, 2);
console.log('proxy', h1.quat.v, h1.quat.calls, log.join(','));

const bare = new Rot();
spin(bare, 0, 5);
console.log('bare', bare._x, Object.prototype.hasOwnProperty.call(bare, '_onChangeCallback'));
