// Numeric-literal keys in object patterns, in every pattern position.
// 14.3.3.1: the key is ToPropertyKey of the literal's value, so `1.0`,
// `0x10` and `1e1` read "1", "16" and "10".

// Declarations: let / const / var.
const { 0: c0, 1: c1 } = ["a", "b"];
console.log("const", c0, c1);
let { 0: l0, 2: l2 = "dflt" } = ["x"];
console.log("let", l0, l2);
var { 1: v1 } = "st";
console.log("var", v1);

// Numeric spellings canonicalize through ToString(value).
const { 1.0: one, 0x10: sixteen, 1e1: ten, 0.5: half, 0b11: three, 0o7: seven } =
    { 1: "one", 16: "sixteen", 10: "ten", 0.5: "half", 3: "three", 7: "seven" };
console.log("spellings", one, sixteen, ten, half, three, seven);

// String-literal and computed keys beside numeric ones.
const k = 4;
const { "2": s2, [k]: comp, [1 + 2]: sum } = ["z0", "z1", "z2", "z3", "z4"];
console.log("mixed", s2, comp, sum);

// Parameters, with and without defaults, in functions and arrows.
function fn({ 0: p0, 1: p1 = "p1d" }) { return p0 + "/" + p1; }
console.log("param", fn(["q"]), fn(["q", "r"]));
const arrow = ({ 0: a0 }, { 1: a1 }) => a0 + a1;
console.log("arrow", arrow("mn", "op"));
const withDefault = ({ 0: d0 } = ["dd"]) => d0;
console.log("param default", withDefault(), withDefault(["ee"]));

// Assignment patterns, plain and inside a larger expression.
let a, b, c;
({ 0: a, 1: b } = ["A", "B"]);
console.log("assign", a, b);
[{ 0: c }] = [["C"]];
console.log("assign nested", c);
console.log("assign value", JSON.stringify(({ 0: a } = ["V"])), a);

// for-of and for-in heads.
for (const { 0: k0, 1: k1 } of [["k", "v"], ["k2", "v2"]]) console.log("for-of", k0, k1);
for (let { 0: ch } of ["ab", "cd"]) console.log("for-of let", ch);
for ({ 0: a, 1: b } of [["e", "f"]]) console.log("for-of assign", a, b);
for (const { 0: first } in { hello: 1, world: 2 }) console.log("for-in", first);

// Nesting: numeric keys inside array and object patterns, patterns as targets.
const nested = { 0: { 1: ["deep"] }, 5: [{ 6: "six" }] };
const { 0: { 1: [deep] }, 5: [{ 6: six }] } = nested;
console.log("nested", deep, six);
const { 0: [n0, { 0: n00 }] } = [["x0", ["x00"]]];
console.log("nested arrays", n0, n00);

// Rest beside numeric keys: the numeric keys are excluded from the rest.
const { 0: r0, ...rest } = { 0: "zero", 1: "one", two: 2 };
console.log("rest", r0, JSON.stringify(rest));
const idx = 1;
const { [idx]: r1, [idx + 1]: r2, ...rest2 } = { 0: "zero", 1: "one", 2: "two", 3: "three" };
console.log("rest computed", r1, r2, JSON.stringify(rest2));

// Getters see the canonical key, and reads happen in source order.
const log = [];
const seen = new Proxy({}, { get(t, key) { log.push(key); return key; } });
const { 3: g3, 1.5: g15, "7": g7, [8]: g8 } = seen;
console.log("order", log.join(","), g3, g15, g7, g8);

// A catch parameter and a class method parameter.
try { throw ["caught"]; } catch ({ 0: err }) { console.log("catch", err); }
class K { m({ 0: mm }) { return mm; } static s({ 1: ss }) { return ss; } }
console.log("class", new K().m(["mv"]), K.s(["_", "sv"]));

// Generator and async function parameters.
function* gen({ 0: gv }) { yield gv; }
console.log("gen", gen(["gy"]).next().value);
(async ({ 0: av }) => { console.log("async", av); })(["ay"]);
