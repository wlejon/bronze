// A fast copy's shape test leaves for the resume twin by each of its three
// tests: a receiver that is no object, one that is no plain object, and a
// plain object of another shape. The loop is hot on one class, and the other
// receivers come rarely and late, each kind in every pass after the first.

class P {
  constructor(x, y) { this.x = x; this.y = y; this.n = 0; }
}

function sum(list, k) {
  let s = 0;
  for (let i = 0; i < list.length; i++) {
    const p = list[i];
    const a = p.x, b = p.y;
    if (a === undefined || b === undefined) { s += k; continue; }
    p.n = a + b;
    s += p.x * 2 + p.y + p.n;
  }
  return s;
}

const list = [];
for (let i = 0; i < 300; i++) list.push(new P(i, i * 0.5));

const arr = [1, 2, 3]; arr.x = 7; arr.y = 8;
function fn() {} fn.x = 3; fn.y = 4;
const swapped = { y: 10, x: 20, n: 0 };
const grown = new P(5, 6); grown.extra = 1;
const getter = { get x() { return 100; }, y: 1, n: 0 };
const odd = [42, 'str', arr, fn, swapped, grown, getter, true, Symbol.iterator];

let total = 0;
for (let r = 0; r < 400; r++) {
  if (r > 0 && r % 50 === 0) {
    const o = odd[(r / 50 - 1) % odd.length];
    list[r % list.length] = o;
    total += sum(list, r);
    list[r % list.length] = new P(r, 1);
  } else {
    total += sum(list, r);
  }
}
console.log(total);
console.log(arr.n, fn.n, swapped.n, grown.n, getter.n);
