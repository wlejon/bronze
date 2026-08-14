// Test inlined dynamic calls (llvm_call.cpp) and proto IC overflow slot reads (llvm_prop.cpp).
// Tests exact arity, over-arity, under-arity padding, this-binding, closures across GC stress,
// prototype overflow methods, and TypeError fallback on non-callable values.

function add2(a, b) {
  return a + b;
}

function pad3(a, b, c) {
  return [a, b === undefined, c === undefined];
}

// 1. Exact arity and over-arity
console.log(add2(10, 20));
console.log(add2(10, 20, 30, 40));

// 2. Under-arity padding fallback
const padded = pad3(42);
console.log(padded[0]);
console.log(padded[1]);
console.log(padded[2]);

// 3. Methods on prototype with overflow slots (> 4 properties on prototype)
function VectorN(x, y, z) {
  this.x = x;
  this.y = y;
  this.z = z;
}
VectorN.prototype.m0 = function() { return 0; };
VectorN.prototype.m1 = function() { return 1; };
VectorN.prototype.m2 = function() { return 2; };
VectorN.prototype.m3 = function() { return 3; };
VectorN.prototype.m4 = function() { return 4; };
VectorN.prototype.m5 = function() { return 5; };
VectorN.prototype.m6 = function() { return 6; };
VectorN.prototype.m7 = function() { return 7; };
VectorN.prototype.dot = function(other) {
  return this.x * other.x + this.y * other.y + this.z * other.z;
};
VectorN.prototype.addScaled = function(other, s) {
  this.x = this.x + other.x * s;
  this.y = this.y + other.y * s;
  this.z = this.z + other.z * s;
  return this;
};

const v1 = new VectorN(1, 2, 3);
const v2 = new VectorN(4, 5, 6);
console.log(v1.m0());
console.log(v1.m4());
console.log(v1.m7());
console.log(v1.dot(v2));

// 4. GC stress test: allocate mid-loop, live references held across calls
const list = [];
let acc = 0;
for (let i = 0; i < 500; i = i + 1) {
  const v = new VectorN(i, i * 2, i * 3);
  v.addScaled(v1, 0.5);
  acc = acc + v.dot(v2);
  if (i % 50 === 0) {
    list.push(v);
  }
}
console.log(list.length);
console.log(acc);
console.log(list[5].x);

// 5. Non-function callee throws TypeError
let threw = false;
try {
  const notAFn = 123;
  notAFn();
} catch (e) {
  threw = true;
}
console.log(threw);
