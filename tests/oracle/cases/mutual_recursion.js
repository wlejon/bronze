// Each function calls the other, so whichever is lowered first reads a
// callee whose body has not been lowered yet — its return type is part of
// the calling convention and has to be settled before either body.
function isEven(n) {
  if (n === 0) {
    return true;
  }
  return isOdd(n - 1);
}

function isOdd(n) {
  if (n === 0) {
    return false;
  }
  return isEven(n - 1);
}

console.log(isEven(10));
console.log(isOdd(10));
console.log(isEven(7));
console.log(isOdd(7));

// A three-function cycle, so no lowering order can see every callee
// first.
function down(n) {
  if (n <= 0) {
    return 0;
  }
  return mid(n - 1) + 1;
}

function mid(n) {
  if (n <= 0) {
    return 0;
  }
  return up(n - 1) + 1;
}

function up(n) {
  if (n <= 0) {
    return 0;
  }
  return down(n - 1) + 1;
}

console.log(down(9));
