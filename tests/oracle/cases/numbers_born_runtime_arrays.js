// Arrays the runtime makes and fills (iterator pairs of Map, Array and typed
// array entries, rest parameters, iterator rest) are born with the NUMBERS or
// NO_CLAIM bit a claim would give them (runtime/array.h settleNumbers). Read
// as Numbers in a trained loop, then mixed values late, then written after
// birth (push of a non-Number, a hole, a length change) and read again.
function sumPairs(it) {
    let s = 0;
    for (const [a, b] of it) s += (typeof a === "number" ? a : 0.25) + (typeof b === "number" ? b : 0.5);
    return s;
}
function rest(...xs) {
    let s = 0;
    for (let i = 0; i < xs.length; i++) s += typeof xs[i] === "number" ? xs[i] : 100;
    return s;
}
function restThenWrite(late, ...xs) {
    if (late) { xs.push("p"); xs[xs.length + 1] = 3; xs.length = 9; }
    let s = "";
    for (let i = 0; i < xs.length; i++) s += xs[i] === undefined ? "u" : xs[i];
    return s;
}
function iterRest(src) {
    const [p, ...more] = src;
    let s = p;
    for (let i = 0; i < more.length; i++) s += typeof more[i] === "number" ? more[i] : 7;
    return s;
}
const out = [];
for (let round = 0; round < 3000; round++) {
    const late = round >= 2950;
    const m = new Map([[1, 2], [3, late ? "four" : 4], [late ? "k" : 5, 6]]);
    const arr = [1, late ? "two" : 2, 3];
    const ta = new Float64Array([1.5, 2.5, 3.5]);
    let r = sumPairs(m) + sumPairs(arr.entries()) + sumPairs(ta.entries()) + sumPairs(m.entries());
    r += rest(1, 2, late ? "x" : 3, 4);
    r += rest();
    const w = restThenWrite(late, 1, 2, 3);
    r += iterRest(late ? [1, "q", 3] : [1, 2, 3]) + iterRest(new Set([4, 5, late ? {} : 6]));
    if (round % 500 === 0 || late) out.push(r + " " + w);
}
console.log(out.join("\n"));
