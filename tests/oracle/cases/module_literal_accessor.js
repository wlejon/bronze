// Reading an accessor property of a module-scope object literal.
//
// `get p() { return this._q; }` on a literal is found as an OWN property of the
// object the literal built, so its `this` is that object and its body is one
// property read of it. This case pins what such a read answers, and pins the
// things that must keep the answer honest whatever a compiler does with it: a
// getter that does more than that one read, a getter that throws, a binding
// that escapes and is then redefined through the alias, a write (which runs the
// SETTER and does not store), a backing property that is deleted underneath the
// accessor, and the parameter-default rules that decide whether the read
// happens on a given call at all.

// --- the plain shape ------------------------------------------------------
// 13.2.5.5: each PropertyDefinition is evaluated in order, and `get`/`set`
// halves of one name are one accessor property, configurable and enumerable.
const Space = {
  _working: 'srgb-linear',
  get working() { return this._working; },
  set working(v) { this._working = v; },
  describe(tag) { return tag + '=' + this._working; }
};

// 10.1.8.1 OrdinaryGet: the accessor is own, so [[Get]] calls the getter with
// the receiver as `this`.
console.log('read          ' + Space.working);
// 13.3.3 and 13.3.2 name the same property, so the bracket form answers the
// same value.
console.log('bracket       ' + Space['working']);
console.log('method this   ' + Space.describe('cs'));

// The same read a thousand times over: an answer that drifts is a cache that
// outlived what it cached.
let hot = 0;
for (let i = 0; i < 1000; i++) {
  if (Space.working === 'srgb-linear') hot++;
}
console.log('hot           ' + hot);

// --- a write goes through the setter --------------------------------------
// 10.1.9.1 OrdinarySetWithOwnDescriptor: an accessor's [[Set]] calls the setter
// with the receiver as `this`. It never stores into the backing property
// directly, so what changes is whatever the setter chose to change.
Space.working = 'display-p3';
console.log('after set     ' + Space.working + ' ' + Space._working);

let cool = 0;
for (let i = 0; i < 1000; i++) {
  if (Space.working === 'display-p3') cool++;
}
console.log('hot after set ' + cool);

// --- a getter that does more than read ------------------------------------
// The increment is observable, so the number of calls is pinned and not just
// the value. A read answered without the call would print calls=0.
let getterCalls = 0;
const Counted = {
  _v: 7,
  get v() { getterCalls++; return this._v; }
};
let vsum = 0;
for (let i = 0; i < 4; i++) vsum += Counted.v;
console.log('counted       ' + vsum + ' calls=' + getterCalls);

// --- a getter that throws -------------------------------------------------
const Boom = {
  _n: 3,
  get boom() { throw new RangeError('boom ' + this._n); }
};
try {
  Boom.boom;
  console.log('boom          NOT REACHED');
} catch (e) {
  console.log('boom          ' + e.name + ' ' + e.message);
}

// --- the backing property deleted underneath the accessor -----------------
// 10.1.8.1 again: with `_q` gone the getter's own read walks to the prototype,
// finds nothing, and answers undefined. Anything standing in for that read has
// to answer the same.
const Backed = { _q: 'here', get q() { return this._q; } };
console.log('backed        ' + Backed.q);
delete Backed._q;
console.log('after delete  ' + Backed.q);
Backed._q = 'again';
console.log('after restore ' + Backed.q);

// --- an escaping binding, redefined through the alias ---------------------
// `Alias` and `Aliased` are the same object, so a redefinition through one is a
// redefinition of the property both read. 10.1.6.3 ValidateAndApplyProperty
// permits it because a literal's accessor is configurable.
const Aliased = { _a: 'A', get a() { return this._a; } };
const Alias = Aliased;
console.log('alias         ' + Alias.a + ' ' + Aliased.a);
Object.defineProperty(Alias, 'a', {
  get() { return 'redefined-' + this._a; },
  configurable: true,
  enumerable: true
});
console.log('after redef   ' + Aliased.a + ' ' + Alias.a);

// --- parameter defaults ---------------------------------------------------
const order = [];
function note(tag, value) { order.push(tag); return value; }

const Def = { _d: 'D', get d() { return this._d; } };

// 10.2.11 FunctionDeclarationInstantiation step 28 and 8.6.3
// IteratorBindingInitialization: a default is evaluated only when the argument
// is undefined, and 8.6.3 makes an explicitly passed `undefined` exactly that.
function pick(space = Def.d) { return String(space); }
console.log('pick()        ' + pick());
console.log('pick(undef)   ' + pick(undefined));
console.log('pick(null)    ' + pick(null));
console.log('pick(value)   ' + pick('given'));

// Left to right, each one only when its own argument is undefined.
function three(a = note('a', 1), b = note('b', 2), c = note('c', 3)) {
  return '' + a + b + c;
}
order.length = 0;
console.log('order all     ' + three() + ' [' + order.join(',') + ']');
order.length = 0;
console.log('order middle  ' + three(9, undefined, 7) + ' [' + order.join(',') + ']');
order.length = 0;
console.log('order last    ' + three(9, 8) + ' [' + order.join(',') + ']');

// An accessor default beside an effectful one: the accessor read is a default
// like any other and happens in its own turn, or not at all.
function pair(x = note('x', 'X'), y = Def.d) { return x + y; }
order.length = 0;
console.log('pair()        ' + pair() + ' [' + order.join(',') + ']');
order.length = 0;
console.log('pair(P,Q)     ' + pair('P', 'Q') + ' [' + order.join(',') + ']');
order.length = 0;
console.log('pair(undef,Q) ' + pair(undefined, 'Q') + ' [' + order.join(',') + ']');
