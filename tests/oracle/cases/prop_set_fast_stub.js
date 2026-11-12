// A cached write site's stub answers an own data slot and an add into an
// inline slot from the site's first way with no call (il_prop_set_fast.cpp)
// and hands the rest to the full stub. Each write below runs at one site
// many times, so the site is cached, and then meets a receiver the cached
// answer must not serve.

function Listener(fn, context, once) {
  this.fn = fn;
  this.context = context;
  this.once = once || false;   // an add after a branch: the stub's
}
function setX(o, v) { o.x = v; }
function addY(o, v) { o.y = v; }

// The add, repeated, then on receivers that are not one add below the cached shape.
var made = [];
for (var i = 0; i < 40; i++) made.push(new Listener(i, { i: i }, i % 3 === 0));
console.log(made.map(function (l) { return l.fn + ':' + l.context.i + ':' + l.once; }).slice(0, 6).join(' '));
console.log(Object.keys(made[39]).join(','), JSON.stringify(made[7]));

var plain = [];
for (var j = 0; j < 30; j++) { var o = { a: j }; addY(o, j * 2); plain.push(o); }
console.log(plain[29].y, Object.keys(plain[29]).join(','));
var already = { a: 1, y: 'old' };
addY(already, 'new');                      // own slot at another shape
var other = { b: 1 };
addY(other, 'o');                          // another parent shape
var frozen = Object.freeze({ a: 1 });
addY(frozen, 'f');                         // not extensible: ignored (sloppy)
var sealed = Object.seal({ a: 1, y: 0 });
addY(sealed, 's');
var prevented = Object.preventExtensions({ a: 1 });
addY(prevented, 'p');
console.log(already.y, other.y, frozen.y, sealed.y, prevented.y, Object.keys(frozen).join(','));
(function () {
  'use strict';
  function addStrict(o, v) { o.y = v; }
  for (var k = 0; k < 20; k++) addStrict({ a: k }, k);
  try { addStrict(Object.freeze({ a: 1 }), 1); console.log('no throw'); } catch (e) { console.log(e instanceof TypeError); }
})();

// The own slot, then a setter, a read-only property and a Proxy at the same site.
var objs = [];
for (var m = 0; m < 30; m++) { var p = { x: 0, tag: m }; setX(p, m); objs.push(p); }
console.log(objs[0].x, objs[29].x);
var log = [];
var withSetter = { set x(v) { log.push('set ' + v); }, tag: 's' };
setX(withSetter, 5);
var readOnly = {};
Object.defineProperty(readOnly, 'x', { value: 'ro', writable: false, enumerable: true });
setX(readOnly, 6);
var proxied = new Proxy({}, { set: function (t, k, v) { log.push('trap ' + String(k) + '=' + v); t[k] = v; return true; } });
setX(proxied, 7);
console.log(log.join(' | '), readOnly.x, proxied.x);

// A setter or a read-only property that appears on the prototype AFTER the add was cached.
function Base() {}
function Derived(v) { this.first = 1; this.late = v; }
Derived.prototype = Object.create(Base.prototype);
for (var n = 0; n < 30; n++) new Derived(n);
var d1 = new Derived('before');
Object.defineProperty(Base.prototype, 'late', { set: function (v) { log.push('proto setter ' + v); }, get: function () { return 'from proto'; }, configurable: true });
var d2 = new Derived('after');
console.log(d1.late, d2.late, Object.keys(d2).join(','), log[log.length - 1]);
delete Base.prototype.late;
Object.defineProperty(Base.prototype, 'late', { value: 'fixed', writable: false, configurable: true });
var d3 = new Derived('ignored');
console.log(d3.late, Object.keys(d3).join(','));
delete Base.prototype.late;
var d4 = new Derived('again');
console.log(d4.late, Object.keys(d4).join(','));

// The receiver is itself a prototype: the add must be seen by its instances.
function addM(o, v) { o.m = v; }
for (var q = 0; q < 30; q++) addM({ k: q }, q);
var protoObj = { k: 'p' };
var inst = Object.create(protoObj);
console.log(inst.m);
addM(protoObj, 'inherited');
console.log(inst.m, Object.keys(protoObj).join(','));

// Many fields: past the inline slots, the adds and the stores still land.
function Wide(v) {
  for (var w = 0; w < 80; w++) setField(this, w, v);
}
function setField(o, w, v) {
  switch (w % 4) {
    case 0: o['f' + w] = v + w; break;
    case 1: o.alpha = (o.alpha || 0) + 1; break;
    case 2: o.beta = v; break;
    default: o.gamma = w;
  }
}
var wide = [new Wide(1), new Wide(2), new Wide(3)];
console.log(Object.keys(wide[2]).length, wide[2].f76, wide[1].alpha, wide[0].beta, wide[2].gamma);
function Fields(v) {
  this.a0 = v; this.a1 = v; this.a2 = v; this.a3 = v; this.a4 = v; this.a5 = v; this.a6 = v; this.a7 = v;
  if (v) { this.b0 = v; this.b1 = v; this.b2 = v; this.b3 = v; this.b4 = v; this.b5 = v; this.b6 = v; this.b7 = v; }
  this.c0 = v ? 1 : 2; this.c1 = [v]; this.c2 = { v: v }; this.c3 = 'end';
}
var fs = [];
for (var r = 0; r < 25; r++) fs.push(new Fields(r));
console.log(Object.keys(fs[0]).length, Object.keys(fs[24]).length, fs[24].b7, fs[24].c1[0], fs[24].c2.v, fs[0].c0, fs[24].c3);

// Receivers that are not plain objects, and values of every kind.
var arr = [1, 2], fnObj = function () {}, ta = new Float32Array(4), str = 'str', num = 5;
[arr, fnObj, ta, str, num, true, new Map(), new Date(0), /re/].forEach(function (recv) { setX(recv, 'v'); });
console.log(arr.x, fnObj.x, ta.x, str.x, num.x);
try { setX(null, 1); } catch (e) { console.log(e instanceof TypeError); }
try { setX(undefined, 1); } catch (e) { console.log(e instanceof TypeError); }
var holder = { x: 0 };
[1.5, -0, NaN, 'text', null, undefined, true, 10n, Symbol.iterator, holder, [1], function () {}].forEach(function (v) {
  setX(holder, v);
  log.push(typeof holder.x + (Object.is(holder.x, -0) ? '-0' : ''));
});
console.log(log.slice(-12).join(','), holder.x === undefined);
