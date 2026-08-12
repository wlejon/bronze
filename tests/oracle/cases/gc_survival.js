// Values held by generated code must survive collections that move them. Every
// live value below spans thousands of allocations; under the gc-stress run
// every one of those allocations moves the whole live set.
const obj = {a: 1, b: 2, c: 3};
const arr = [10, 20, 30];
const view = new Float32Array(4);
view[0] = 0.5;
view[3] = 2.25;
let str = "kept";

let i = 0;
let sum = 0;
while (i < 3000) {
  const churn = {x: i, y: i + 1};
  sum = sum + churn.x + churn.y;
  i = i + 1;
}

console.log(sum);
console.log(obj.a + obj.b + obj.c);
console.log(arr[0] + arr[1] + arr[2]);
console.log(view[0] + view[3]);
console.log(str);

// The same again, with the live values now also flowing through a call.
function tally(o, a) {
  let t = 0;
  let k = 0;
  while (k < 500) {
    const tmp = {v: k};
    t = t + tmp.v;
    k = k + 1;
  }
  return t + o.a + a[2];
}

console.log(tally(obj, arr));

str = str + "-and-kept";
let j = 0;
while (j < 3000) {
  const more = [j, j + 1];
  j = j + 1;
}
console.log(str);
console.log(str.length);
console.log(obj.c);

// The runtime caches the charCodeAt function object forever; that cache is
// a root that outlives every frame, and the churn below moves it.
console.log(str.charCodeAt(0));
let m = 0;
while (m < 1000) {
  const z = "x" + m;
  m = m + 1;
}
console.log(str.charCodeAt(5));
