// A nested function that calls itself. Its own name is an env-backed
// binding of the enclosing scope, so the self-call resolves through the
// environment chain rather than through a direct call.
function outer() {
  function fact(n) {
    if (n < 2) { return 1; }
    return n * fact(n - 1);
  }
  return fact(6);
}
console.log(outer());

// Mutual recursion between two nested functions: `isOdd` is referenced by
// `isEven` before it is declared, which only works because both slots are
// allocated when the scope is entered.
function classify(n) {
  function isEven(k) {
    if (k === 0) { return true; }
    return isOdd(k - 1);
  }
  function isOdd(k) {
    if (k === 0) { return false; }
    return isEven(k - 1);
  }
  return isEven(n);
}
console.log(classify(10));
console.log(classify(7));
