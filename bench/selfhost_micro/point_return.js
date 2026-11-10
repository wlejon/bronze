// A function returning {x, y}, consumed field by field by its caller.
function polar(r, t) {
    return { x: r * Math.cos(t), y: r * Math.sin(t) };
}
let sx = 0, sy = 0;
for (let i = 0; i < 10000000; i++) {
    const p = polar(i & 1023, i & 7);
    sx += p.x;
    sy += p.y;
}
console.log(Math.round(sx) + Math.round(sy));
