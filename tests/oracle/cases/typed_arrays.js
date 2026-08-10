const v = new Float32Array(8);
console.log(v.length);
v[0] = 1.5;
v[1] = 2.25;
v[7] = -3.75;
console.log(v[0]);
console.log(v[1]);
console.log(v[7]);
console.log(v[3]);

let sum = 0;
for (let i = 0; i < v.length; i++) {
  sum = sum + v[i];
}
console.log(sum);

v[100] = 9;
console.log(v[100]);

const buf = new ArrayBuffer(16);
console.log(buf.byteLength);
const w = new Float32Array(buf);
console.log(w.length);
w[2] = 0.5;
console.log(w[2]);

const prec = new Float32Array(1);
prec[0] = 0.1;
console.log(prec[0]);

const arr = [10, 20, 30];
let k = 1;
console.log(arr[k]);
arr[k] = 99;
console.log(arr[1]);
arr[k] += 1;
console.log(arr[k]);
