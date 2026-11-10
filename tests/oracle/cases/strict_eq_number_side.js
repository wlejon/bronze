// `x === n` with one side known to be a Number (a typed value, or a box of
// one) and the other any Value: one double compare answers it
// (il_lowering_ops.cpp, strict_eq_number_side). NaN equals nothing, +0 and -0
// are equal, and no other kind equals a Number, however its bits look.

function show(label, v) { console.log(label, String(v)); }

const others = [0, -0, 1, 1.5, -1, NaN, Infinity, -Infinity, 2 ** 53, "0", "1", "", " ", true, false,
                null, undefined, 0n, 1n, {}, [], [0], Symbol.iterator, () => 0, new Number(1), new String("1")];

function eqLeft(x, k) { const n = k * 1; return n === x; }
function eqRight(x, k) { const n = k + 0; return x === n; }
function neRight(x, k) { const n = k / 1; return x !== n; }

const nums = [0, -0, 1, NaN, -1, 1.5, Infinity];
for (let r = 0; r < 3; ++r) {
  for (const k of nums) {
    let line = "";
    for (const x of others) line += (eqLeft(x, k) ? "1" : "0") + (eqRight(x, k) ? "1" : "0") + (neRight(x, k) ? "1" : "0") + " ";
    show("n=" + String(k), line);
  }
}

// From a closure (the case that motivated it): `some(v => v === target)`.
function finder(target) {
  const t = target + 0;
  return (arr) => [arr.some((v) => v === t), arr.find((v) => v === t), arr.findIndex((v) => v === t),
                   arr.filter((v) => v === t).length, arr.indexOf(t), arr.includes(t)].join();
}
const mixed = [1, "1", 2, -0, NaN, true, null, undefined, 1n, [1], 3.25];
for (let r = 0; r < 3; ++r) {
  for (const t of [1, 0, -0, NaN, 3.25, 7]) show("finder " + String(t), finder(t)(mixed));
}

// A loop with a number accumulator compared to array elements.
function countEqual(arr, n) {
  let c = 0;
  for (let i = 0; i < arr.length; ++i) if (arr[i] === n + 0) ++c;
  return c;
}
const big = [];
for (let i = 0; i < 1000; ++i) big.push(i % 3 === 0 ? i % 7 : i % 3 === 1 ? String(i % 7) : { v: i % 7 });
for (let r = 0; r < 3; ++r) show("count", [0, 1, 2, 3, 4, 5, 6].map((n) => countEqual(big, n)).join());
