function fibStep(a, b) {
  return a + b;
}
function fib10(a, b) {
  const f1 = a + b;
  const f2 = b + f1;
  const f3 = f1 + f2;
  const f4 = f2 + f3;
  const f5 = f3 + f4;
  const f6 = f4 + f5;
  const f7 = f5 + f6;
  const f8 = f6 + f7;
  const f9 = f7 + f8;
  const f10 = f8 + f9;
  return f10;
}
function fib100(a, b) {
  const a1 = fib10(a, b);
  const a2 = fib10(b, a1);
  const a3 = fib10(a1, a2);
  const a4 = fib10(a2, a3);
  const a5 = fib10(a3, a4);
  const a6 = fib10(a4, a5);
  const a7 = fib10(a5, a6);
  const a8 = fib10(a6, a7);
  const a9 = fib10(a7, a8);
  const a10 = fib10(a8, a9);
  return a10;
}
function fib1000(a, b) {
  const b1 = fib100(a, b);
  const b2 = fib100(b, b1);
  const b3 = fib100(b1, b2);
  const b4 = fib100(b2, b3);
  const b5 = fib100(b3, b4);
  const b6 = fib100(b4, b5);
  const b7 = fib100(b5, b6);
  const b8 = fib100(b6, b7);
  const b9 = fib100(b7, b8);
  const b10 = fib100(b8, b9);
  return b10;
}
console.log(fib1000(0, 1));
