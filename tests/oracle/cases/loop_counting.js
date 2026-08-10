function countAndSum(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) {
    sum += i;
  }
  return sum;
}

function whileCount(n) {
  let count = 0;
  let i = n;
  while (i > 0) {
    count++;
    i--;
  }
  return count;
}

console.log(countAndSum(10));
console.log(whileCount(5));
