// Arms that leave a binding at different types: the join parameter has to
// describe both, so it can be neither arm's own type.
function label(n) {
  let out = 0;
  if (n > 0) {
    out = "pos";
  } else {
    out = n * 2;
  }
  return out;
}
console.log(label(3));
console.log(label(-4));

let t = 0;
let flag = true;
if (flag) {
  t = "yes";
} else {
  t = 1;
}
console.log(t);
console.log(t + "!");

// An `if` with no else joins the changed arm against the untouched
// entry value.
let u = 9;
if (flag) {
  u = "nine";
}
console.log(u);
