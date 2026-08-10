function testTernary(a, b) {
  const max = a > b ? a : b;
  const label = a === b ? "equal" : a > b ? "greater" : "lesser";
  return label + ": " + max;
}

console.log(testTernary(10, 20));
console.log(testTernary(30, 15));
console.log(testTernary(5, 5));
