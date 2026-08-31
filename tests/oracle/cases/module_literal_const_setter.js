// The setter of a module-scope object literal, invoked.
//
// A setter's body is unreachable code exactly while nothing writes ITS key, and
// that is what lets the write it performs be discounted when deciding whether
// the property it writes is constant. Here the key IS written — `Space.v = 'b'`
// — so the setter runs, its write lands, and the getter that reads the backing
// property afterwards sees it. Nothing about `_v` is constant, and a read that
// answered the literal's initializer would print `a` on every line after the
// first.
//
// The setter is not the identity: it stores `x + '!'`, so the value read back
// is not the value assigned, and a compiler that folded the ASSIGNED value into
// the following read would be wrong in a second, different way (10.1.9.1 —
// [[Set]] on an accessor calls the setter and stores nothing itself, so what
// the property holds is entirely the setter's business).
//
// `Twin` pins the other half of the reachability argument in the same program:
// its key `w` is never written, so its setter never runs — and its backing `_w`
// is written by nothing else either. The two literals differ only in whether
// their accessor is assigned, and the run must not be able to tell that from
// the values it reads.

const Space = {
  _v: 'a',
  get v() { return this._v; },
  set v(x) { this._v = x + '!'; }
};

const Twin = {
  _w: 'a',
  get w() { return this._w; },
  set w(x) { this._w = x + '!'; }
};

console.log('initial       ' + Space.v + ' ' + Space._v + ' | ' + Twin.w + ' ' + Twin._w);

// 10.1.9.1 OrdinarySetWithOwnDescriptor: an own accessor's [[Set]] calls the
// setter with the receiver as `this` and stores nothing directly.
Space.v = 'b';

console.log('after set     ' + Space.v + ' ' + Space._v + ' | ' + Twin.w + ' ' + Twin._w);

let hot = 0;
let twinHot = 0;
for (let i = 0; i < 500; i++) {
  if (Space.v === 'b!') hot++;
  if (Twin.w === 'a') twinHot++;
}
console.log('hot           ' + hot + ' ' + twinHot);

// A second write through the same accessor: the setter runs again, on the value
// it stored last time only if the body reads it — this one does not.
Space.v = Space.v;
console.log('set from self ' + Space.v + ' ' + Space._v);

// The assignment expression's VALUE is the right-hand side, not what the setter
// stored (13.15.2: the result of a simple assignment is `rval`).
const assigned = (Space.v = 'c');
console.log('assign value  ' + assigned + ' ' + Space.v);
