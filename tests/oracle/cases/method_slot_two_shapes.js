// One call site over receivers of two (then three) shapes that each OWN the
// method as a closure — three.js's `renderer.setMode(...)` over its buffer
// and indexed-buffer renderers. Each receiver's own function must run, through
// alternation, replacement, deletion and a third shape.
function Plain(tag) {
  let calls = 0;
  function render(n) { calls++; return tag + ":" + n + ":" + calls; }
  this.render = render;
}
function Indexed(tag) {
  let calls = 0;
  function setIndex() {}
  function render(n) { calls++; return tag + "#" + n + ":" + calls; }
  this.setIndex = setIndex;
  this.render = render;
}
const p = new Plain("p"), q = new Indexed("q"), q2 = new Indexed("q2");
function draw(r, n) { return r.render(n); }
const out = [];
for (let i = 0; i < 12; i++) out.push(draw(i % 3 === 0 ? p : (i % 3 === 1 ? q : q2), i));
console.log(out.join(" "));
// Replaced on one instance: the site re-reads the slot.
q.render = function (n) { return "replaced" + n; };
console.log(draw(q, 1), draw(p, 2), draw(q2, 3), draw(q, 4));
// Deleted: falls to the prototype chain (none here), so a TypeError.
delete q2.render;
try { draw(q2, 5); } catch (e) { console.log(e.constructor.name); }
console.log(draw(p, 6), draw(q, 7));
// A third shape, and a method that moves to a prototype.
class Proto { render(n) { return "proto" + n; } }
const r = new Proto();
const mix = [];
for (let i = 0; i < 9; i++) mix.push(draw([p, q, r][i % 3], i));
console.log(mix.join(" "));
Proto.prototype.render = function (n) { return "patched" + n; };
console.log(draw(r, 1), draw(p, 2), draw(q, 3));
