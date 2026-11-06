// A method key that names a prototype no-op AND is written as an own property
// (three.js's `_onChangeCallback`: Quaternion's empty prototype method, which
// `_onChange(cb)` shadows with a closure on each instance). The build guesses
// the prototype method as the direct target and the site also dispatches the
// receiver's own slot: receivers with a closure of their own run it, receivers
// without one run the prototype method, and either may change under the site.
// The closures read their captured variables, `this`, `arguments` or nothing;
// the prototype method is replaced by one that reads `this`; an own slot is
// overwritten with a non-function and deleted again. Whatever the build
// guessed, the bytes are these.

const out = [];
const log = (...xs) => out.push(xs.join(" "));

class Quat {
  constructor(id) { this.id = id; this.x = 0; this.y = 0; }
  _onChange(cb) { this._onChangeCallback = cb; return this; }
  _onChangeCallback() {}
  fromArray(a, o) {
    this.x = a[o];
    this.y = a[o + 1];
    this._onChangeCallback();
    return this;
  }
  set(x, y) {
    this.x = x;
    this.y = y;
    this._onChangeCallback(x, y);
    return this;
  }
}

class Holder {
  constructor(id) {
    const q = new Quat(id);
    const self = this;
    this.q = q;
    this.hits = 0;
    this.sum = 0;
    function onQuatChange() { self.hits++; self.sum += q.x + q.y; }
    q._onChange(onQuatChange);
  }
}

const holders = [];
for (let i = 0; i < 6; ++i) holders.push(new Holder(i));
const bare = [new Quat(100), new Quat(101)];
const data = [];
for (let i = 0; i < 64; ++i) data.push(i * 0.5);

function run(n, list) {
  let acc = 0;
  for (let i = 0; i < n; ++i) {
    const q = list[i % list.length];
    q.fromArray(data, (i * 2) % 62);
    acc += q.x - q.y;
  }
  return acc;
}

function stats() {
  return holders.map((h) => h.hits + "/" + h.sum).join(",");
}

const quats = holders.map((h) => h.q);
log("warm", run(600, quats), stats());
log("bare", run(100, bare), bare.map((q) => q.x + ":" + q.y).join(","));
log("mixed", run(300, quats.concat(bare)), stats());

// A closure that reads `this` and one that reads `arguments`.
quats[1]._onChange(function () { this.tagged = (this.tagged || 0) + 1; });
quats[2]._onChange(function () { this.argc = arguments.length; });
log("this/arguments", run(120, quats), quats[1].tagged, quats[2].argc, stats());
quats[2].set(3, 4);
log("set", quats[2].argc, quats[2].x, quats[2].y);

// The prototype method replaced by one that reads `this`.
let protoCalls = 0;
Quat.prototype._onChangeCallback = function () { protoCalls += this.id; };
log("proto replaced", run(40, bare), protoCalls);

// An own slot cleared back to the prototype, then made a non-function.
delete quats[3]._onChangeCallback;
log("deleted", run(60, quats), protoCalls, stats());
quats[4]._onChangeCallback = 7;
let caught = "";
try { quats[4].fromArray(data, 0); } catch (e) { caught = e instanceof TypeError ? "TypeError" : String(e); }
log("not callable", caught, quats[4].x);

// An arrow closure capturing a counter, and an accessor holding the callback.
let arrowCount = 0;
quats[5]._onChange(() => { arrowCount++; });
Object.defineProperty(quats[0], "_onChangeCallback", { get() { return () => { arrowCount += 100; }; }, configurable: true });
log("arrow/accessor", run(60, [quats[0], quats[5]]), arrowCount);

log("final", stats(), protoCalls, arrowCount);
console.log(out.join("\n"));
