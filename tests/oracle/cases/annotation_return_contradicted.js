// A return annotation the body contradicts. `label(): number` returning a
// string used to give the function an f64 return type, so the `return`
// unboxed the string and the caller read a double out of it.
function label(): number {
  return "forty-two";
}

// Two returns of different types: the annotation names one of them, and
// neither is what the caller can rely on. The joined return is dynamic, so
// both paths keep their own value (ECMA-262 puts no constraint on a
// function's return type at all).
export function pick(c): number {
  if (c) {
    return 1;
  }
  return "no";
}

console.log(label());
console.log(pick(1));
console.log(pick(0));
