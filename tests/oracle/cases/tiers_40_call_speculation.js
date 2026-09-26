// Call-target speculation (src/codegen-brass/il2mir/il_lowering_calls.cpp)
// and guards in functions with an argv block. `Math.sqrt`/`abs`/`floor`/
// `ceil` called as methods tier up to the machine op behind a guard on the
// callee's code pointer and a Number argument; then the target changes (a
// monkey-patched member, another receiver) or the argument stops being a
// Number, and each failure finishes the call in Tier 0. The functions with
// method calls of several arguments carry an argv block, which a resume
// re-creates in the Tier-0 frame. Every tier must print the same thing, with
// and without BRASS_DEOPT_STRESS.

function norm(x, y, z) {
  return Math.sqrt(x * x + y * y + z * z);
}
function rounders(v) {
  return Math.floor(v) + Math.ceil(v) * 2 + Math.abs(v) * 3;
}
function viaReceiver(m, v) {
  return m.sqrt(v);
}

let acc = 0;
for (let i = 0; i < 20000; i++) acc += norm(i % 5, i % 7, 1);
console.log(acc.toFixed(6));
for (let i = 0; i < 20000; i++) acc += rounders((i % 11) - 5.5);
console.log(acc.toFixed(6));

// Arguments that are not Numbers, and results the fast path must box right.
console.log(Math.sqrt(-4), norm('3', 4, '0'), rounders('2.5'), rounders(undefined));
console.log(1 / Math.floor(-0.5), 1 / Math.ceil(-0.5), 1 / Math.abs(-0), Math.abs(-Infinity));
console.log(rounders({ valueOf() { return -1.25; } }), norm(null, true, 2));

// Another receiver with a `sqrt` of its own.
let vr = 0;
for (let i = 0; i < 20000; i++) vr += viaReceiver(Math, i);
console.log(vr.toFixed(4));
const fake = { sqrt(v) { return v * 10; } };
console.log(viaReceiver(fake, 3), viaReceiver(Math, 81));
for (let i = 0; i < 30; i++) vr += viaReceiver(i % 2 ? fake : Math, i);
console.log(vr.toFixed(4));

// The builtin itself replaced after tier-up, then restored.
const realSqrt = Math.sqrt;
let calls = 0;
Math.sqrt = function (v) { calls++; return -v; };
console.log(norm(1, 2, 2), calls);
let patched = 0;
for (let i = 0; i < 5000; i++) patched += norm(i % 3, 0, 0);
console.log(patched, calls);
Math.sqrt = realSqrt;
console.log(norm(2, 3, 6));

// Guards in a function with an argv block: a method call of three
// arguments stages them in an alloca, live across the property and
// arithmetic sites, whose failures resume in a fresh Tier-0 frame.
function Pt(x, y) { this.x = x; this.y = y; }
function collect(list, p, q) {
  list.push({ v: p.x + q.y }, p.x * q.x, [p.y]);
  return p.x + p.y + q.x + q.y;
}
const list = [];
let cs = 0;
for (let i = 0; i < 20000; i++) {
  cs += collect(list, new Pt(i, 1), new Pt(2, i));
  if (list.length > 300) list.length = 0;
}
console.log(cs, list.length, list[0].v, list[2][0]);
console.log(collect(list, { y: 'y', x: 'x' }, new Pt(1, 2)), list[list.length - 3].v);
console.log(collect(list, new Pt(1.5, 2), { x: 3, y: { valueOf() { return 4; } } }));
for (let i = 0; i < 20000; i++) cs += collect(list, new Pt(i, 1), i % 2 ? new Pt(2, i) : { x: 2, y: i });
console.log(cs, list[list.length - 3].v);
