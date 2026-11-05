// A plain array's element read is a Number only when the array holds one. The
// coerced form of the read (ToNumber of the element) is exact only where the
// operator converts with ToNumeric and uses nothing else of the value: never
// for a store, a `+`, a relational compare of two strings, or a `+=` into a
// plain array whose element may be a string.

const obj = { valueOf() { return 3; }, n: 3 };
const odd = ["5", obj, 7, undefined, null, true, "x"];
odd[9] = "9"; // index 8 is a hole

// Stores into number arrays built by push and by literal.
const pushed = [];
for (let i = 0; i < 6; i++) pushed.push(i);
const literal = [0, 1, 2, 3, 4, 5];
for (const xs of [pushed, literal]) {
  xs[0] = odd[0];
  xs[1] = odd[1];
  xs[2] = odd[2];
  xs[3] = odd[8];
  xs[4] = odd[4];
  xs[5] = odd[5];
  console.log(xs.map((v) => typeof v).join(","));
  console.log(xs[1] === obj, xs[0] === "5", xs[3] === undefined, 3 in xs, xs[4] === null);
}

// Store through computed indices in a loop.
const sink = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
for (let i = 0; i < odd.length; i++) sink[i] = odd[i];
console.log(sink.map((v) => (v === obj ? "obj" : typeof v + ":" + String(v))).join(" "));
console.log(8 in odd, 8 in sink);

// The assignment expression's value is the RHS itself.
const r = (pushed[2] = odd[1]);
console.log(r === obj, typeof r);

// A typed array target converts, but the expression value does not.
const ta = new Float64Array(3);
const r2 = (ta[0] = odd[1]);
const r3 = (ta[1] = ta[9]);
console.log(ta[0], r2 === obj, r3);

// `+` with a numeric other side still concatenates a string element.
const strs = ["2", "3", "10", "9"];
const one = 1;
console.log(one + strs[0], strs[0] + strs[1], strs[1] + one);
console.log(one + odd[1], odd[1] + one, odd[2] + one);

// Relational: two strings compare as strings.
console.log(strs[2] < strs[3], strs[2] > strs[3], strs[2] <= strs[3], strs[2] >= strs[3]);
console.log(strs[2] < 9, one < strs[3], odd[1] < 4);

// Arithmetic always coerces.
console.log(strs[0] * strs[1], strs[2] - one, odd[1] * 2, -odd[0], odd[8] * 1);

// `+=` into a plain array keeps the string branch; `-=` coerces.
const ws = ["a", "5", obj, 4];
ws[0] += 1;
ws[1] += 1;
ws[2] += 1;
ws[3] += 1;
console.log(ws.map((v) => typeof v + ":" + v).join(" "));
const ms = ["8", obj, 4];
ms[0] -= 1;
ms[1] -= 1;
ms[2] -= 1;
console.log(ms.map((v) => typeof v + ":" + v).join(" "));

// Typed-array `+=` with a string element.
const tb = new Float64Array([5, 6]);
tb[0] += strs[1];
tb[1] += odd[1];
console.log(tb[0], tb[1]);

// Index from a plain array of strings: a property key, not a ToNumber.
const nums = [10, 20, 30];
const keys = ["1", { toString() { return "2"; }, valueOf() { return 0; } }];
console.log(nums[keys[0]], nums[keys[1]]);

// A relational result is a Boolean, not a number.
let flag = 1;
const lim = 2;
flag = flag < lim;
console.log(typeof flag, flag);
function counter() {
  let z = 0;
  const step = () => { z = z < 5; };
  step();
  return typeof z + ":" + z;
}
console.log(counter());

// Increment coerces the element.
const inc = ["4", obj];
inc[0]++;
inc[1]++;
console.log(inc[0], inc[1]);

// A binding from a typed element used in a plain-array store.
const tc = new Float64Array(2);
const plain = [1, 2];
for (let i = 0; i < 3; i++) {
  const v = tc[i + 1];
  plain[i] = v;
}
console.log(plain.map((v) => typeof v).join(","));

// The original shape: an untouched literal of a string and an object.
const odd2 = ["5", { valueOf() { return 3; }, n: 3 }];
const xs2 = [];
for (let i = 0; i < 4; i++) xs2.push(i);
xs2[3] = odd2[1];
console.log(typeof xs2[3], xs2[3] === odd2[1], xs2[3].n);
xs2[2] = odd2[0];
console.log(typeof xs2[2], xs2[2] + 1);
