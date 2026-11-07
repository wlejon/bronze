// Object.defineProperty with a key that is a loop variable (a block
// parameter, not an instruction): the repeated-read pass's scan for getter
// keys must not read it as an instruction.
function f(n) {
    const g = {};
    for (let i = 0; i < 16; i++) Object.defineProperty(g, i, { get() { return 1; } });
    return g;
}
console.log(f(1)[0]);
function make(n) {
    const g = {};
    for (let i = 0; i < n; i++) {
        Object.defineProperty(g, i, { get() { return i * 2; }, enumerable: true });
    }
    for (let k = 0; k < n; k++) {
        Object.defineProperty(g, "k" + k, { value: k, enumerable: true });
    }
    return g;
}
const g = make(4);
console.log(g[0], g[1], g[3], g.k2, Object.keys(g).join(","));
let sum = 0;
for (let r = 0; r < 3; r++) sum += g[2] + g[2];
console.log(sum);
