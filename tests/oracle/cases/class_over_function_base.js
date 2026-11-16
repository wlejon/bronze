// Classes whose base is a constructor FUNCTION (an ES5 EventEmitter under a
// class tree, as eventemitter3 sits under pixi): the class layout of such a
// chain is unproven, and the instances are given room for every name the
// derived classes write (fields, constructor and method this-writes) plus a
// default for what the base adds. The names must land where reads find them
// whether the base adds few fields, many, returns another object, is
// swapped for another function, or had its prototype object replaced, and
// the objects behave the same once they outgrow the room.
function Emitter() { this._events = {}; this._count = 0; }
Emitter.prototype.on = function (n, f) { (this._events[n] ||= []).push(f); this._count++; return this; };
Emitter.prototype.emit = function (n, v) { const l = this._events[n]; if (l) for (const f of l) f(v); return !!l; };
function Wide() { for (let i = 0; i < 12; i++) this['w' + i] = i; }
function Other() { return { replaced: true, x: -1 }; }
class Node extends Emitter {
  label = 'node';
  constructor(x) { super(); this.x = x; this.y = x * 2; this.vx = 0.5; this.vy = -0.5; }
  step() { this.x += this.vx; this.y += this.vy; this.moved = true; return this; }
}
class Sprite extends Node {
  tint = 0xffffff;
  constructor(x) { super(x); this.alpha = 1; this.scale = 1.5; for (let i = 0; i < 8; i++) this['extra' + i] = i * x; }
  get area() { return this.scale * this.scale; }
  set area(a) { this.scale = Math.sqrt(a); this.areaSet = a; }
}
class WideKid extends Wide { constructor() { super(); this.k = 'kid'; this.w3 = 'mine'; } }
class Odd extends Other { constructor() { super(); this.after = 2; } }
const Base = Emitter;
class Late extends (function pick() { return Base; })() { constructor() { super(); this.late = 1; } }
function Replaced() { this.bound = 'yes'; }
Replaced.prototype = { greet() { return 'hi ' + this.own; } };
class FromBound extends Replaced { constructor() { super(); this.own = 3; } }

const out = [];
const sprites = [];
for (let i = 0; i < 2000; i++) {
  const s = new Sprite(i % 10);
  s.on('tick', (v) => { s.x += v; });
  s.step().emit('tick', 1);
  if (i % 3 === 0) s.area = 4;
  sprites.push(s);
}
let sum = 0;
for (const s of sprites) sum += s.x + s.y + s.alpha + s.scale + s.extra7 + s._count + (s.moved ? 1 : 0) + s.area;
out.push(sum.toFixed(3), sprites[3].label, sprites[3].tint, Object.keys(sprites[5]).join());
for (let i = 0; i < 40; i++) sprites[7]['more' + i] = i;
out.push(sprites[7].more39, sprites[7].vx, sprites[7].extra3, Object.keys(sprites[7]).length);
delete sprites[8].vx; sprites[8].step();
out.push(String(sprites[8].x), sprites[8].hasOwnProperty('vx'));
const w = new WideKid();
out.push(Object.keys(w).join(), w.w3, w.w11);
const o = new Odd();
out.push(o instanceof Odd, o.replaced, o.after, o.x);
out.push(new Late().late, Object.keys(new Late()).join());
Emitter.prototype.extraProto = 'p';
const n = new Node(3);
out.push(n.extraProto, Object.keys(n).join(), n.on('a', () => {})._count);
const fb = new FromBound();
out.push(fb.bound, fb.own, fb instanceof FromBound, fb.greet());
console.log(out.join('\n'));
