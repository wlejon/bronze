// A builtin's bare name reads the global object's property of that name, so
// writing the property rebinds the name: `globalThis.Math = m` makes `Math`
// mean `m` everywhere, including in code that read `Math` before the write
// and in loops whose `Math.sqrt` was compiled as a call.
function sum(n) { let s = 0; for (let k = 0; k < n; k++) s += Math.sqrt(k) + Math.abs(k - 5); return s; }
console.log("math", Math.round(sum(1000)));

const realMath = Math;
globalThis.Math = { sqrt: () => 1, abs: () => 2 };
console.log("patched", sum(10), Math === realMath, globalThis.Math === Math);
globalThis.Math = realMath;
console.log("restored", Math.round(sum(10)), Math === realMath);

// A method of the real object replaced, then put back.
const realSqrt = Math.sqrt;
Math.sqrt = (x) => -x;
console.log("method", sum(4));
Math.sqrt = realSqrt;
console.log("method back", Math.round(sum(4)));

// Other builtins, by assignment and by definition.
const realJSON = JSON;
globalThis.JSON = { stringify: () => "fake" };
console.log("json", JSON.stringify({ a: 1 }));
globalThis.JSON = realJSON;
console.log("json back", JSON.stringify({ a: 1 }));

Object.defineProperty(globalThis, "parseInt", { value: () => 99, writable: true, configurable: true });
console.log("parseInt", parseInt("5"));
let reads = 0;
Object.defineProperty(globalThis, "parseFloat", { get() { reads++; return () => reads; }, configurable: true });
console.log("parseFloat getter", parseFloat("1"), parseFloat("1"), reads);

// The binding as seen through globalThis and by name agree.
globalThis.Map = class FakeMap { get size() { return -1; } };
console.log("map", new Map().size, globalThis.Map === Map);
