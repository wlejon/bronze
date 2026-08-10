function classify(val) {
  if (val < 0) {
    return "negative";
  } else if (val === 0) {
    return "zero";
  } else if (val < 10) {
    return "small";
  } else if (val < 100) {
    return "medium";
  } else {
    return "large";
  }
}

console.log(classify(-5));
console.log(classify(0));
console.log(classify(7));
console.log(classify(42));
console.log(classify(500));
