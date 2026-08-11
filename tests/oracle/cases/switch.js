// `switch`: selection by strict equality, then fallthrough (docs/0018
// decision 4). What makes it more than sugar for a chain of `if`s:
//
// 1. The match is STRICT equality on the discriminant (ECMA-262 14.12.10
//    uses IsStrictlyEqual), so `1` and `"1"` select different cases and
//    `true` selects neither.
// 2. Control enters at the matching case and RUNS ON through the following
//    ones until a `break` — `g(1)` appends both "a" and "b".
// 3. `default` is not "the last resort at the bottom": it is selected only
//    when no case matches, but execution then falls through whatever
//    physically follows it. `mid(5)` runs the default and then case 1's
//    body, printing "D1".
//
// The block-argument SSA of docs/0005 is the shape this needs: one block per
// case body, with a fallthrough edge from each to the next.
function f(v) {
  switch (v) {
    case 1:
      return "one";
    case 2:
    case 3:
      return "two-or-three";
    default:
      return "other";
  }
}
console.log(f(1));
console.log(f(2));
console.log(f(3));
console.log(f(4));
function g(v) {
  let out = "";
  switch (v) {
    case 1:
      out = out + "a";
    case 2:
      out = out + "b";
      break;
    case 3:
      out = out + "c";
      break;
    default:
      out = out + "d";
  }
  return out;
}
console.log(g(1));
console.log(g(2));
console.log(g(3));
console.log(g(9));
function h(v) {
  switch (v) {
    case 1:
      return "num";
    case "1":
      return "str";
    default:
      return "none";
  }
}
console.log(h(1));
console.log(h("1"));
console.log(h(true));
function mid(v) {
  let out = "";
  switch (v) {
    default:
      out = out + "D";
    case 1:
      out = out + "1";
      break;
    case 2:
      out = out + "2";
  }
  return out;
}
console.log(mid(1));
console.log(mid(2));
console.log(mid(5));
