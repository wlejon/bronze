function testShortCircuit() {
  console.log(0 || "x");
  console.log(1 && 2);
  console.log("a" || "b");
  console.log("" || 42);
  console.log(0 && "foo");
  console.log("bar" && 0);

  // Nullish coalescing operator
  console.log(0 ?? 5);
  console.log(null ?? 5);
  console.log(undefined ?? 5);
  console.log("" ?? 5);
  console.log(false ?? 5);
  console.log(NaN ?? 5);
}

testShortCircuit();
