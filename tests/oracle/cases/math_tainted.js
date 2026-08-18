// The other side of the pristine-Math proof: one write through `Math`
// anywhere in the module forfeits it for the whole program, and a binding
// NAMED Math shadows it locally. Every call here must take the dynamic path
// in both modes — the pin is that the taint scan and the shadow check stand
// the fast path down without changing a single byte of behaviour.

Math.myExtra = 5;
console.log(Math.sqrt(16), Math.myExtra);

// A replaced method is the caller's problem, not the compiler's: the write
// above already tainted the module, so this call reads the property like any
// other and reaches the replacement.
Math.sqrt = function (n) { return "not-sqrt:" + n; };
console.log(Math.sqrt(25));

// A local binding named Math shadows the builtin entirely.
function shadowed() {
  const Math = { sqrt: function (n) { return "shadow:" + n; } };
  return Math.sqrt(9);
}
console.log(shadowed());

// The untouched functions still work through the (tainted) dynamic path.
console.log(Math.abs(-8), Math.floor(3.9));
