// Many values live across calls and loop back edges at once, so most live
// in stack slots: rotations and swaps of loop-carried values (parallel
// copies whose sources and destinations are spilled, including cycles),
// doubles and tagged values alive across the same calls, values dead
// before a call next to values live after it, and guard chains on objects
// whose shape changes mid-loop.
function id(x) { return x; }
function touch(o) { return o.k === undefined ? 0 : 1; }
function rotate(n) {
  let a = 1, b = 2, c = 3, d = 4, e = 5, f = 6, g = 7, h = 8, i = 9, j = 10, k = 11, l = 12;
  for (let r = 0; r < n; r++) {
    const t = a; a = b; b = c; c = d; d = e; e = f; f = g; g = h; h = i; i = j; j = k; k = l; l = id(t);
    if (r % 3 === 0) { const s = a; a = l; l = s; }
  }
  return [a, b, c, d, e, f, g, h, i, j, k, l].join(",");
}
console.log(rotate(1), rotate(7), rotate(100), rotate(1001));
function swaps(n) {
  let x = 0.5, y = -1.25, z = 3, w = "w", p = { v: 1 }, q = [2];
  let s = 0;
  for (let r = 0; r < n; r++) {
    [x, y] = [y, x];
    [z, w] = [w, z];
    [p, q] = [q, p];
    s += id(x) * 2 - y + (typeof z === "number" ? z : z.length) + touch(p);
  }
  return `${x} ${y} ${z} ${w} ${JSON.stringify(p)} ${JSON.stringify(q)} ${s}`;
}
console.log(swaps(0), "|", swaps(1), "|", swaps(10), "|", swaps(333));
function mixed(n) {
  let f0 = 0.1, f1 = 0.2, f2 = 0.3, f3 = 0.4, f4 = 0.5, f5 = 0.6, f6 = 0.7, f7 = 0.8;
  let t0 = "a", t1 = { n: 1 }, t2 = [1, 2], t3 = null, t4 = 7, t5 = true;
  let out = 0;
  for (let r = 0; r < n; r++) {
    f0 += f1 * f2; f1 -= f3 / (f4 + 1); f2 = f5 * f6 - f7; f3 = id(f0) + f2;
    f4 = Math.sqrt(f4 * f4 + 1); f5 = -f5; f6 += 0.001; f7 = f7 * 0.999;
    t0 = id(t0 + r % 10).slice(-4); t1 = { n: t1.n + 1 }; t2 = t2.concat(r).slice(-3);
    t3 = r % 2 ? t3 : t1; t4 = (t4 * 31 + r) % 1000003; t5 = !t5;
    out += f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7;
  }
  return [out.toFixed(6), t0, t1.n, t2.join("-"), t3 && t3.n, t4, t5].join(" ");
}
console.log(mixed(1));
console.log(mixed(500));
function args() {
  let s = arguments.length;
  for (let i = 0; i < arguments.length; i++) s += id(arguments[i]) * (i + 1);
  return s;
}
console.log(args(), args(1), args(1, 2, 3), args.apply(null, Array.from({ length: 40 }, (_, i) => i)));
// One guard chain site over objects that change shape and kind.
function gx(o) { return o.x + o.y; }
const shapes = [{ x: 1, y: 2 }, { y: 2, x: 1 }, { x: "1", y: 2 }, Object.create({ x: 5, y: 6 }), [0], { x: 1, y: 2, z: 3 }];
let g = "";
for (let r = 0; r < 2000; r++) { const o = shapes[r % shapes.length]; g = String(gx(o)); if (r === 1000) delete shapes[0].y; }
console.log(g, shapes.map(gx).join(","));
// Recursion with many live locals around the call.
function deep(n, a, b, c) {
  if (n === 0) return a + b * c;
  const u = a + 1, v = b * 1.5, w2 = c - 0.25, z = String(n);
  const r = deep(n - 1, v, w2, u);
  return r + u + v + w2 + z.length;
}
console.log(deep(30, 1, 2, 3).toFixed(4));
