// A loop whose step is merged from a conditional and whose counter starts at
// that step, with a call in the body. The merge block only forwards the step
// to the loop header, and brass's cfg_simplify bypassed it as an identity
// trampoline although the body reads the step again: the optimizing tier
// failed with "SSA Dominance violation ... block parameter of block 'b3'",
// and with the verifier off it ran wrong code.
function pushes(c, out) {
    const step = c > 16 ? 5 : 1;
    for (let k = step; k < 30; k += step) out.push(k);
    return out.length;
}
function calls(c, g) {
    const step = c > 16 ? 5 : 1;
    let n = 0;
    for (let k = step; k < 30; k += step) n += g(k);
    return n;
}
const id = (k) => k;
console.log(pushes(30, []), pushes(3, []));
console.log(calls(30, id), calls(3, id));
