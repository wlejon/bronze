function matrixSum(rows, cols) {
  let total = 0;
  for (let r = 0; r < rows; r++) {
    for (let c = 0; c < cols; c++) {
      total += r * cols + c;
    }
  }
  return total;
}

console.log(matrixSum(3, 4));
