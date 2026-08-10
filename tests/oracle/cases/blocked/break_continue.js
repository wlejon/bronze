function testBreakContinue(limit) {
  let sum = 0;
  for (let i = 0; i < limit; i++) {
    if (i % 2 === 0) {
      continue;
    }
    if (i > 7) {
      break;
    }
    sum += i;
  }
  return sum;
}

console.log(testBreakContinue(15));
