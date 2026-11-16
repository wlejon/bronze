// Math functions held in variables, the way three.js's setFromEuler holds
// them (`const cos = Math.cos, sin = Math.sin`): the hot calls take the
// computed form, and a call with non-number arguments still converts them as
// the call says.
function euler(x, y, z, cos, sin) {
    const c1 = cos(x / 2), c2 = cos(y / 2), c3 = cos(z / 2);
    const s1 = sin(x / 2), s2 = sin(y / 2), s3 = sin(z / 2);
    return s1 * c2 * c3 + c1 * s2 * s3 + c1 * c2 * s3 - s1 * s2 * c3;
}
// The same body, for the calls that hand it other functions: the hot
// function's sites only ever held Math.cos and Math.sin.
function eulerAny(x, y, z, cos, sin) {
    const c1 = cos(x / 2), c2 = cos(y / 2), c3 = cos(z / 2);
    const s1 = sin(x / 2), s2 = sin(y / 2), s3 = sin(z / 2);
    return s1 * c2 * c3 + c1 * s2 * s3 + c1 * c2 * s3 - s1 * s2 * c3;
}
function main() {
    let s = 0;
    for (let i = 0; i < 200000; i++) s += euler(i * 0.001, i * 0.002, i * 0.003, Math.cos, Math.sin);
    console.log(s.toFixed(9));
    // Strings convert, an object's valueOf runs once per call.
    let calls = 0;
    const o = { valueOf() { calls++; return 0.5; } };
    console.log(euler('1', o, 2, Math.cos, Math.sin).toFixed(12), calls);
    // Another function in the variable.
    console.log(eulerAny(1, 2, 3, (v) => v * 10, Math.sin).toFixed(12));
    console.log(eulerAny(1, 2, 3, Math.cos, Math.tan).toFixed(12));
    const pow2 = Math.pow;
    let t = 0;
    for (let i = 0; i < 1000; i++) t += pow2(1.001, i);
    console.log(t.toFixed(9), pow2(2), pow2());
}
main();
