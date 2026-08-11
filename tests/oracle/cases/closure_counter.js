// The canonical mutable capture: state that outlives the call that made it.
function makeCounter() {
  let n = 0;
  return function () {
    n = n + 1;
    return n;
  };
}
const c = makeCounter();
console.log(c());
console.log(c());
console.log(c());
const d = makeCounter();
console.log(d());
console.log(c());
