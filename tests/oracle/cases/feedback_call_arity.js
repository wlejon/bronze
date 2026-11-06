// Method sites whose profiled callee declares fewer parameters than the call
// passes (three.js's `onBeforeRender(renderer, scene, camera, ...)` on an
// empty prototype method): the direct edge drops the extra arguments, which
// the callee cannot observe. Then the callee is replaced by functions that do
// observe them (`arguments`, a rest parameter, more declared parameters), an
// instance gets its own method, receivers of new shapes arrive, and short
// calls pad with undefined. Whatever the build guessed, the bytes are these.

const out = [];
const log = (...xs) => out.push(xs.join(" "));

class Base {
  constructor(id) { this.id = id; this.calls = 0; }
  onBefore() { this.calls++; }
  onAfter(r) { this.calls += r === undefined ? 100 : 1; }
  pad(a, b, c) { return [a, b, c].map(String).join("|"); }
}
class KindA extends Base { constructor(id) { super(id); this.a = 1; } }
class KindB extends Base { constructor(id) { super(id); this.b = 2; this.bb = 3; } }
class KindC extends Base { constructor(id) { super(id); this.c = 4; } }
class KindD extends Base { constructor(id) { super(id); this.d = 5; this.dd = 6; this.ddd = 7; } }

const objs = [new KindA(0), new KindB(1), new KindC(2), new KindD(3)];

function render(o, r, s, c, g, m, grp) {
  o.onBefore(r, s, c, g, m, grp);
  o.onAfter(r, s, c, g, m, grp);
  return o.pad(r);
}

function frame(n, list) {
  let last = "";
  for (let i = 0; i < n; ++i) last = render(list[i % list.length], i, "s", "c", "g", "m", null);
  return last + ":" + list.map((o) => o.calls).join(",");
}

function reset() { for (const o of objs) o.calls = 0; }

log("warm", frame(400, objs));
reset();
log("warm2", frame(400, objs));

// The prototype method now reads `arguments`: every argument the call passed.
reset();
Base.prototype.onBefore = function () { this.calls += arguments.length; };
log("arguments", frame(8, objs));

// A rest parameter collects the extras.
reset();
Base.prototype.onBefore = function (...rest) { this.calls += rest.length * 10; };
log("rest", frame(8, objs));

// More declared parameters than before: the sixth argument is seen.
reset();
Base.prototype.onBefore = function (r, s, c, g, m, grp) { this.calls += grp === null ? 7 : 1; };
log("declared", frame(8, objs));

// Back to an empty method, and one instance's own method observes the extras.
reset();
Base.prototype.onBefore = function () { this.calls++; };
objs[2].onBefore = function () { this.calls += 1000 * arguments.length; };
log("own", frame(8, objs));
delete objs[2].onBefore;

// A subclass override on the prototype of one kind.
reset();
KindB.prototype.onBefore = function (r, s) { this.calls += s === "s" ? 50 : -50; };
log("override", frame(8, objs));
delete KindB.prototype.onBefore;

// New shapes the profile never saw.
reset();
class KindE extends Base { constructor(id) { super(id); this.e = 8; } }
const more = objs.concat([new KindE(4), Object.assign(new KindA(5), { extra: 1 })]);
log("new shapes", frame(12, more));

// Short calls pad with undefined; extras are dropped.
reset();
log("short", objs[0].pad(), objs[1].pad(1), objs[2].pad(1, 2), objs[3].pad(1, 2, 3, 4, 5));
Base.prototype.onAfter = function (r, s, c, g, m, grp, extra) { this.calls += extra === undefined ? 3 : 0; };
log("padded", frame(8, objs));

// A plain function value called with extras, then replaced with one reading them.
let hook = function () { return 0; };
function callHook(i) { return hook(i, i + 1, i + 2); }
let sum = 0;
for (let i = 0; i < 300; ++i) sum += callHook(i);
log("hook warm", sum);
hook = function (a, b, c) { return a + b + c; };
sum = 0;
for (let i = 0; i < 4; ++i) sum += callHook(i);
log("hook replaced", sum);
hook = function () { return arguments.length; };
log("hook arguments", callHook(1));

console.log(out.join("\n"));
