// The same back-edge type change in a three-part `for`, where the loop
// binding also has to survive the update block's join.
let acc = 0;
for (let i = 0; i < 3; i = i + 1) {
  if (i === 1) {
    acc = "one";
  }
}
console.log(acc);

// `+` with a string on the left is concatenation, and ToString(Number)
// of an integral double has no decimal point.
let n = 5;
for (let j = 0; j < 2; j = j + 1) {
  n = "j" + j;
}
console.log(n);

// A loop counter whose type never changes stays a number throughout.
let total = 0;
for (let k = 0; k < 4; k = k + 1) {
  total = total + k;
}
console.log(total);
