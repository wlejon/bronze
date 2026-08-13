// %TypedArray%.prototype.toString (23.2.3.31) is Array.prototype.toString (23.1.3.33),
// which invokes the receiver's `join` method or falls back to Object.prototype.toString.

// Basic typed array toString calls join
console.log(new Float32Array([1, 2, 3]).toString());
console.log(new Uint8Array([10, 20, 30]).toString());
console.log(new Int32Array().toString());

// Array.prototype.toString calls join
console.log([1, 2, 3].toString());
console.log([].toString());
console.log([1, null, undefined, 4].toString());

// Function identity (23.2.3.31)
console.log(Array.prototype.toString === new Float32Array().toString);
console.log(Array.prototype.toString === [].toString);

// Coercions via ToPrimitive
console.log("" + new Float32Array([1.5, 2.5]));
console.log(String(new Float32Array([10, 20])));

// Array.prototype.toString on custom object with join
console.log(Array.prototype.toString.call({ join: function () { return "custom"; } }));

// Array.prototype.toString on object where join is non-callable falls back to Object.prototype.toString
console.log(Array.prototype.toString.call({ join: 123 }));

// Array.prototype.toString on null / undefined throws TypeError
try {
  Array.prototype.toString.call(null);
} catch (e) {
  console.log(e.name);
}

try {
  Array.prototype.toString.call(undefined);
} catch (e) {
  console.log(e.name);
}
