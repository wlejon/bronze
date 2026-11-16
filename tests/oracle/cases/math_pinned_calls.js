// Math is pinned by lockdown: `Math.sin(x)` on the global Math, and a call of
// a value read from it (`const cos = Math.cos`), are computed with no read of
// the property and no identity test. The arguments still convert as the call
// says when they are not Numbers (the site leaves to the generic function),
// assigning over the builtin is refused, and a lexical `Math` is not the
// builtin.
function step(n, t) {
    let s = 0;
    for (let i = 0; i < n; i++) {
        const ph = i * 0.25;
        s += Math.sin(t + ph) * 0.2 + Math.cos(t * 1.3 + ph) + Math.atan2(ph, 1 + t) + Math.sqrt(ph);
    }
    return s;
}
function fromEuler(x, y, z) {
    const cos = Math.cos, sin = Math.sin;
    const c1 = cos(x / 2), c2 = cos(y / 2), c3 = cos(z / 2);
    const s1 = sin(x / 2), s2 = sin(y / 2), s3 = sin(z / 2);
    return s1 * c2 * c3 + c1 * s2 * s3 - s1 * s2 * c3 + Math.abs(c1 * c2) + Math.max(s1, s3) + Math.imul(7, 3);
}
function mixed(v) {
    return Math.floor(v) + Math.sin(v);
}
function shadowed(Math) {
    return Math.sin(2);
}
function main() {
    let a = 0;
    for (let f = 0; f < 400; f++) a += step(100, f * 0.016);
    console.log(a.toFixed(9));
    let b = 0;
    for (let i = 0; i < 100000; i++) b += fromEuler(i * 0.001, i * 0.002, i * 0.003);
    console.log(b.toFixed(9));
    let c = 0;
    for (let i = 0; i < 100000; i++) c += mixed(i * 0.5);
    console.log(c.toFixed(9));
    // Not Numbers: converted by the call, valueOf runs once.
    let calls = 0;
    const o = { valueOf() { calls++; return 0.5; } };
    console.log(mixed('1.5').toFixed(12), mixed(o).toFixed(12), calls, mixed(null).toFixed(12), mixed(undefined));
    console.log(fromEuler('1', o, true).toFixed(12), calls);
    // The builtin cannot be replaced, nor the binding rebound.
    try { Math.sin = () => 42; } catch (e) { console.log('assign', e instanceof TypeError); }
    try { globalThis.Math = { sin: () => 43 }; } catch (e) { console.log('rebind', e instanceof TypeError); }
    console.log(mixed(1).toFixed(12), Math.sin === Math.sin, typeof Math.sin);
    // A parameter named Math is whatever it is handed.
    let d = 0;
    for (let i = 0; i < 20000; i++) d += shadowed({ sin: (v) => v + i });
    console.log(d);
}
main();
