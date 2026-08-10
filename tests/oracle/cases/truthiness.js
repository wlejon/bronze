function testTruthiness(val) {
  if (val) {
    return "truthy";
  } else {
    return "falsy";
  }
}

console.log(testTruthiness(0));
console.log(testTruthiness(-0));
console.log(testTruthiness(NaN));
console.log(testTruthiness(""));
console.log(testTruthiness("0"));
console.log(testTruthiness(undefined));
console.log(testTruthiness(null));
console.log(testTruthiness({}));
console.log(testTruthiness([]));
