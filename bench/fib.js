// Call-heavy benchmark: naive recursive fibonacci.
import { measure } from './harness.js';

function fib(n) {
  if (n < 2) {
    return n;
  }
  return fib(n - 2) + fib(n - 1);
}

console.log(measure('fib', () => fib(30)));
