// `a.push(v)` on an array the function made becomes array.push behind an
// is.plain_array guard in a speculated build (speculate/array_push.cpp). The
// guard fails, and G makes the call, when the array has left the bare shape.
function filter(list, drop) {
    for (var i = 0, out = [], n = list.length; i < n; i++) {
        if (list[i].v !== drop) out.push(list[i]);
    }
    return out;
}
function collect(n, tagAt, freezeAt, protoAt) {
    const out = [];
    for (let i = 0; i < n; i++) {
        if (i === tagAt) out.tag = "named";            // leaves the bare shape
        if (i === freezeAt) Object.freeze(out);        // push then throws
        if (i === protoAt) Object.setPrototypeOf(out, { push(v) { this.pushed = (this.pushed || 0) + 1; return -1; } });
        const r = out.push(i * 2);
        if (r !== -1 && r !== out.length) throw new Error("bad length " + r);
    }
    return out;
}
function grow(n) {
    const a = new Array(3);
    for (let i = 0; i < n; i++) a.push(i);
    return a;
}
function mixed(n) {
    const a = [];
    for (let i = 0; i < n; i++) a.push(i % 3 === 0 ? "s" + i : i % 3 === 1 ? i + 0.5 : { i });
    return a;
}
const list = [];
for (let i = 0; i < 50; i++) list.push({ v: i % 7 });
let sum = 0;
for (let round = 0; round < 3000; round++) {
    sum += filter(list, round % 7).length;
    sum += collect(20, -1, -1, -1).length;
    sum += grow(40).length;
    sum += mixed(10).length;
}
console.log(sum);
const tagged = collect(10, 4, -1, -1);
console.log(tagged.length, tagged.tag, tagged.join());
try {
    collect(10, -1, 5, -1);
} catch (e) {
    console.log(e instanceof TypeError);
}
const proto = collect(6, -1, -1, 3);
console.log(proto.length, proto.pushed, Array.isArray(proto));
const g = grow(5);
console.log(g.length, 0 in g, g[3], g.join());
const m = mixed(7);
console.log(m.length, typeof m[0], m[1], m[2].i);
// A push whose value is the array itself, and the result used.
function selfPush() { const a = []; let r = 0; for (let i = 0; i < 4; i++) r += a.push(a); return [r, a[3] === a, a.length]; }
console.log(selfPush().join());
