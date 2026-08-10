function testDoWhile() {
  let count = 0;
  let i = 0;
  do {
    count += i;
    i++;
  } while (i < 5);

  let runOnce = 0;
  do {
    runOnce++;
  } while (false);

  return count * 100 + runOnce;
}

console.log(testDoWhile());
