// A fresh object or array literal's values answer the reads of its slots and
// elements (opt/redundancy/value_walk.cpp forwardLiteral) until a write kills
// them. Literals of 2..9 keys (overflow slots past the inline four, and a
// large literal sized inline), number-slot keys trained on Numbers and fed
// other values late, and every writer between the literal and the read: a
// direct store, an alias, a call, delete, defineProperty, a setter on the
// prototype path cannot apply (own data properties), Object.assign.
function small(i, v) {
    const o = { a: i, b: v, c: [i, v] };
    return o.a + o.c[1];
}
function wide(i) {
    const o = { k0: i, k1: i + 1, k2: i + 2, k3: i + 3, k4: i + 4, k5: i + 5, k6: i + 6, k7: i + 7, k8: "z" + i };
    return o.k0 + o.k4 + o.k6 + o.k8;
}
function written(i, late) {
    const o = { x: i, y: i * 2 };
    const alias = late ? o : { x: 0, y: 0 };
    alias.x = "w";
    return o.x + o.y;
}
function called(i, f) {
    const o = { x: i, y: 1 };
    const arr = [i, i + 1, i + 2];
    f(o, arr);
    return o.x + "/" + arr[1];
}
function deleted(i, late) {
    const o = { p: i, q: i + 1, r: i + 2 };
    if (late) delete o.q;
    return o.p + "|" + o.q + "|" + o.r;
}
function defined(i, late) {
    const o = { m: i, n: 2 };
    if (late) Object.defineProperty(o, "m", { value: "d", writable: false });
    if (late) Object.assign(o, { n: "assigned" });
    return o.m + ":" + o.n;
}
function arrays(i, late) {
    const a = [i, i + 0.5, i * 3];
    const b = late ? a : [0, 0, 0];
    b[1] = "e";
    a[2] = late ? null : a[2];
    return a[0] + "," + a[1] + "," + a[2];
}
const out = [];
for (let round = 0; round < 3000; round++) {
    const late = round >= 2950;
    const v = late && round % 2 ? "s" : round;
    const r = [small(round, v), wide(round), written(round, late),
               called(round, late ? (o, a) => { o.x = "c"; a[1] = "c"; } : () => {}),
               deleted(round, late), defined(round, late), arrays(round, late)];
    if (round % 500 === 0 || late) out.push(r.join(" "));
}
console.log(out.join("\n"));
