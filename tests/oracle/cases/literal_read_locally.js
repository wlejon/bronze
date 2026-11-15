// Literals read locally: every answer must match node.
const out = [];
function swap(a, b) { [a, b] = [b, a]; return a * 10 + b; }
function pick(i) { const o = { a: i, b: i + 1, c: [i, i * 2] }; return o.a + o.b + o.c[1] + o.c[0]; }
function absent(i) { const o = { a: i }; return o.zz === undefined ? o.a : -1; }
function protoRead(i) { const o = { a: i }; return typeof o.toString + o.a; }
function lengthRead(i) { const t = [i, i, i]; return t.length + t[2]; }
function dup(i) { const o = { a: i, a: i + 5 }; return o.a; }
function protoKey(i) { const p = { v: i }; const o = { __proto__: p, w: 1 }; return o.v + o.w; }
function escape(i) { const o = { a: i }; out.length; return [o, o.a]; }
function later(i) { const o = { a: i }; o.a = i + 1; o.b = 2; return o.a + o.b; }
function hole() { const t = [1, , 3]; return String(t[1]) + t[2]; }
function nested(i) { const o = { p: { q: { r: i } }, s: [[i], [i + 1]] }; return o.p.q.r + o.s[1][0]; }
function cond(i) { const o = { a: i, b: 2 }; if (i & 1) return o.a; let s = 0; for (let k = 0; k < 3; k++) s += o.b; return s; }
function destr(i) { const { x, y } = { x: i, y: i * 2 }; const [p, q] = [x + 1, y + 1]; return x + y + p + q; }
function accessor(i) { const o = { get a() { return i * 3; }, b: i }; return o.a + o.b; }
function method(i) { const o = { f() { return this.k; }, k: i }; return o.f(); }
function spread(i) { const o = { ...{ a: i }, b: 1 }; return o.a + o.b; }
function computed(i) { const k = 'a'; const o = { [k]: i, b: 1 }; return o.a + o.b; }
function strKey(i) { const o = { 'x-y': i, 0: 7 }; return o['x-y'] + o[0]; }
function loop() { let s = 0; for (let i = 0; i < 1000; i++) { const v = { x: i, y: [i, 1] }; s += v.x + v.y[0] + v.y[1]; } return s; }
function thrower(i) { try { const o = { a: i, b: (() => { if (i > 2) throw new Error('e' + i); return 1; })() }; return o.a + o.b; } catch (e) { return e.message; } }
for (let i = 0; i < 5; i++) {
  out.push(swap(i, i + 1), pick(i), absent(i), protoRead(i), lengthRead(i), dup(i), protoKey(i), JSON.stringify(escape(i)),
    later(i), hole(), nested(i), cond(i), destr(i), accessor(i), method(i), spread(i), computed(i), strKey(i), thrower(i));
}
out.push(loop());
console.log(out.join('\n'));
