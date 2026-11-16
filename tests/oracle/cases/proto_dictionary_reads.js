// A prototype made a dictionary by many keys (a WebGL context's enums,
// `P[k] = C[k]` past the dictionary threshold) answers reads through it from a
// cache (Shape::protoLinkCacheable): valid while the prototype epoch holds, so
// every change to the dictionary that a read could see must still be seen:
// a value write, a delete, a re-add, an accessor redefinition, an own shadow
// on the receiver or on a middle prototype, a prototype swap, a freeze.
class Ctx { constructor(n) { this.n = n; } }
const P = Ctx.prototype;
for (let i = 0; i < 700; i++) P["K" + i] = i;
class Sub extends Ctx {}
function readA(c) { return c.K5 + c.K600; }
function readB(c) { return c.K42; }
function readMiss(c) { return c.NOPE === undefined ? 1 : 2; }
const out = [];
const objs = [new Ctx(1), new Sub(2), new Ctx(3)];
for (let round = 0; round < 3000; round++) {
    let s = 0;
    for (const o of objs) s += readA(o) + readB(o) + readMiss(o);
    if (round === 2900) P.K5 = 1000;
    if (round === 2910) delete P.K42;
    if (round === 2920) P.K42 = "back";
    if (round === 2930) Object.defineProperty(P, "K600", { get() { return -1; }, configurable: true });
    if (round === 2940) objs[0].K5 = 7;
    if (round === 2950) Sub.prototype.K42 = "mid";
    if (round === 2960) P.NOPE = 5;
    if (round === 2970) Object.setPrototypeOf(Sub.prototype, { K5: 9, K42: 8, K600: 6 });
    if (round === 2980) Object.freeze(P);
    if (round % 500 === 0 || round >= 2895) out.push(round + ":" + s);
}
console.log(out.join("\n"));
