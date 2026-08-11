// A `while` loop whose back edge changes a binding's type. The loop
// header's binding for `x` is a number on the entry edge and a string on
// the back edge, so neither edge's type describes the merge (docs/0005
// decision 2, docs/0010 decision 3).
let x = 1;
let more = true;
while (more) {
  x = "s";
  more = false;
}
console.log(x);
console.log(x + "!");

// The type changes more than once, at different points of the loop.
let v = 0;
let i = 0;
while (i < 4) {
  if (i === 0) {
    v = "a";
  }
  if (i === 2) {
    v = 10;
  }
  i = i + 1;
}
console.log(v);
console.log(i);
