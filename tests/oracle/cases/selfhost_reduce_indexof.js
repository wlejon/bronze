// reduce's initial-value rule and indexOf's fromIndex conversion and
// equality, exhaustively.
"use strict";

function err(label, f) {
    try { console.log(label, f()); }
    catch (e) { console.log(label, e.constructor.name + ": " + e.message); }
}

err("reduce empty", () => [].reduce((a, x) => a + x));
err("reduce empty with initial", () => [].reduce((a, x) => a + x, "init"));
err("reduce explicit undefined initial", () => [1, 2].reduce((a, x) => String(a) + x, undefined));
err("reduce all holes", () => [, , ,].reduce((a, x) => a + x));
err("reduce all holes with initial", () => [, , ,].reduce((a, x) => a + x, 0));
err("reduce one element", () => [42].reduce(() => { throw new Error("not called"); }));
err("reduce first after holes", () => [, , 3, 4].reduce((a, x, i) => a + ":" + i + "=" + x));
err("reduce indices", () => ["a", "b", "c"].reduce((a, x, i, arr) => a + i + x + arr.length, ""));
err("reduce extra args ignored", () => [1, 2].reduce((a, x) => a + x, 10, 20));
err("reduce on array-like empty", () => Array.prototype.reduce.call({ length: 0 }, () => 1));
err("reduce on array-like holes", () => Array.prototype.reduce.call({ length: 3, 1: "only" }, () => "called"));
err("reduce not callable first", () => [].reduce(5));
err("reduceRight still", () => [1, 2, 3].reduceRight((a, x) => a + x));

const values = [0, -0, 1, "1", NaN, null, undefined, false, "", [], {}];
const arr = [0, 1, "1", NaN, null, undefined, false, ""];
for (const v of values) console.log("indexOf", Object.is(v, -0) ? "-0" : JSON.stringify(v) ?? String(v), arr.indexOf(v));
console.log("indexOf -0 finds +0", [0].indexOf(-0), [-0].indexOf(0));
const o = {};
console.log("indexOf object identity", [{}, o].indexOf(o), [{}].indexOf({}));

const ten = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
const froms = [undefined, 0, 3, -3, 9, 10, 11, -10, -11, 2.7, -2.7, "4", " 4 ", "x", NaN, Infinity, -Infinity,
    null, true, [5], 1e21, -1e21, 2 ** 32, -(2 ** 32)];
for (const f of froms) {
    console.log("fromIndex", typeof f === "number" || f === undefined ? String(f) : JSON.stringify(f),
        ten.indexOf(5, f), ten.indexOf(0, f), ten.indexOf(9, f));
}
console.log("fromIndex -0 result is +0", Object.is([1].indexOf(1, -0), 0));
let conv = 0;
console.log("fromIndex valueOf", ten.indexOf(7, { valueOf() { conv++; return 6; } }), conv);
err("fromIndex throws", () => [1].indexOf(1, { valueOf() { throw new URIError("from"); } }));
err("fromIndex symbol", () => [1].indexOf(1, Symbol()));
err("fromIndex bigint", () => [1].indexOf(1, 1n));
console.log("indexOf length 2 slots", [1, 2].indexOf(2, 1), [1, 2].indexOf(1, 1));
console.log("indexOf no argument", [undefined].indexOf(), [1].indexOf());
const big = [];
for (let i = 0; i < 1000; i++) big.push(i * 2);
console.log("indexOf big", big.indexOf(1998), big.indexOf(1999), big.indexOf(500, -600));
const strs = ["a", "b", "c"];
console.log("indexOf strings", strs.indexOf("c"), strs.indexOf("b" + ""), strs.indexOf(new String("a")));
