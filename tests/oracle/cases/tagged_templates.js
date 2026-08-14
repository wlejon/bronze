// Tagged template literals: tag function invocation, cooked strings, raw strings, and freeze state.

function passthrough(strings, ...values) {
  console.log("Strings length:", strings.length);
  console.log("Values count:", values.length);
  console.log("Is frozen strings:", Object.isFrozen(strings));
  console.log("Is frozen raw:", Object.isFrozen(strings.raw));
  console.log("Raw strings:", strings.raw.join("|"));
  
  let result = "";
  for (let i = 0; i < strings.length; i = i + 1) {
    result = result + strings[i];
    if (i < values.length) {
      result = result + values[i];
    }
  }
  return result;
}

const name = "World";
const answer = 42;
const res = passthrough`Hello ${name}! The answer is ${answer}.`;
console.log("Result:", res);

function rawViewer(strings) {
  return strings.raw[0];
}

const rawRes = rawViewer`Line1\nLine2\tTab`;
console.log("Raw length:", rawRes.length);
console.log("Raw includes backslash n:", rawRes.indexOf("\\n") !== -1);

function mathTag(strings, ...values) {
  let sum = 0;
  for (const v of values) {
    sum = sum + v;
  }
  return sum;
}

const x = 10, y = 20, z = 30;
const total = mathTag`Adding ${x} and ${y} plus ${z}`;
console.log("Total sum:", total);
