// A module-level function whose name is only ever the callee of a call has no
// unknown callers, so its parameter and return types are joined over every call
// site and its calls become direct TYPED calls. `fib` is the shape that
// motivated it — every site passes a number, so the parameter, the return and
// the recursive call are all unboxed f64 with no box/unbox pair anywhere on the
// path.
//
// This case pins the behaviour by output rather than by IL: a typed call
// that got the convention wrong would read the wrong register, and a `Never`
// or a wrong signature would show up as a wrong number here.

function fib(n) {
  if (n < 2) {
    return n;
  }
  return fib(n - 2) + fib(n - 1);
}

function fact(n) {
  if (n <= 1) {
    return 1;
  }
  return n * fact(n - 1);
}

// Two numeric parameters, called both from the top level and from inside
// another typed function — the call-graph join has to see both sites.
function poly(x, k) {
  return x * x + k * x - 1;
}

function sumPoly(count) {
  let total = 0;
  for (let i = 0; i < count; i++) {
    total = total + poly(i, 2);
  }
  return total;
}

// A proven Bool return: a typed direct call that is not an f64 one, and a
// recursion whose signature the call-graph fixpoint has to settle before
// the body that contains the self-call is lowered.
function isEvenDown(n) {
  if (n === 0) {
    return true;
  }
  if (n === 1) {
    return false;
  }
  return isEvenDown(n - 2);
}

// A name read as a value escapes, so decision 5's test rejects it and it
// keeps the uniform dynamic convention however numeric its body looks.
// Both the aliased call and the by-name call must still be correct.
function twice(x) {
  return x + x;
}
const alias = twice;

for (let i = 0; i < 10; i++) {
  console.log(fib(i));
}
console.log(fact(10));
console.log(poly(3, 4));
console.log(sumPoly(5));
console.log(isEvenDown(10));
console.log(isEvenDown(7));
console.log(isEvenDown(0));
console.log(alias(21));
console.log(twice(1.5));
console.log(twice("a"));
