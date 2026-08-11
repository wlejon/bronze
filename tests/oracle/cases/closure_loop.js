// A block-scoped binding declared inside a loop body is a FRESH binding on
// every iteration, so each closure captures its own.
const fns = [0, 0, 0];
let i = 0;
while (i < 3) {
  const k = i * 10;
  fns[i] = function () { return k; };
  i = i + 1;
}
console.log(fns[0]());
console.log(fns[1]());
console.log(fns[2]());
