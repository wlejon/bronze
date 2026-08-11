// The type changes on only some paths through the body, so the join at
// the loop header is what proves it can be either.
function pick(flag) {
  let out = 0;
  let i = 0;
  while (i < 3) {
    if (flag) {
      out = "s" + i;
    } else {
      out = out + 1;
    }
    i = i + 1;
  }
  return out;
}
console.log(pick(true));
console.log(pick(false));

// `continue` and `break` edges carry the changed type too: one leaves
// from the middle of the body, the other from the end.
let z = 0;
let c = 0;
while (c < 5) {
  c = c + 1;
  if (c === 2) {
    z = "two";
    continue;
  }
  if (c === 4) {
    z = "four";
    break;
  }
}
console.log(z);
console.log(c);
