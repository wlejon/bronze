// Double slots that turn over (runtime/slot_repr.h): a key trained on Numbers
// (eligible, read as a Number behind its shape cell) whose objects then
// alternate a string and a Number many times, through the trained store and
// read sites. Each object leaves the double shape once (generalization) and
// stays boxed; objects made after it are born boxed (the double edge is
// sticky). The reads must see every value as written; the objects end on
// Numbers so the harvest still calls the key one.
class P { constructor(x, y) { this.px = x; this.py = y; } }
function put(o, v) { o.px = v; }
function get(o) { return o.px; }
function norm(o) { return o.px * o.px + o.py * o.py; }
const show = (v) => typeof v === 'number' ? String(v) : typeof v + ':' + String(v);
const ps = [];
for (let i = 0; i < 64; i++) ps.push(new P(i + 0.5, 1));
let acc = 0;
for (let k = 0; k < 3000; k++) {
  for (const p of ps) { put(p, p.px + 0.25); acc += norm(p) + get(p); }
}
console.log('trained', acc);
let text = '';
for (let k = 0; k < 400; k++) {
  const p = ps[k & 63];
  put(p, (k & 1) ? 's' + k : k * 0.5);
  const v = get(p);
  if (k % 37 === 0) text += show(v) + ' ' + show(norm(p)) + '\n';
  acc += typeof v === 'number' ? v : v.length;
}
console.log(text.trimEnd());
const late = [];
for (let i = 0; i < 8; i++) late.push(new P(i % 2 ? 'q' : i, 2));
for (const p of late) console.log(show(get(p)), show(norm(p)));
for (const p of ps) put(p, 1.5);
for (const p of late) put(p, 2.5);
for (let k = 0; k < 3000; k++) for (const p of ps) acc += norm(p) + get(p);
console.log('after', acc);
