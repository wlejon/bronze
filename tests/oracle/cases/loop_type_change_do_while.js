// A `do`-`while` runs its body before the condition, so the binding
// reaches the condition block's join with the body's type and the
// header's with the entry type.
let x = 1;
let k = 0;
do {
  x = "run" + k;
  k = k + 1;
} while (k < 3);
console.log(x);
console.log(k);

// The other direction: a string on entry, a number on the back edge.
let y = "start";
let m = 0;
do {
  y = m * 2;
  m = m + 1;
} while (m < 2);
console.log(y);
