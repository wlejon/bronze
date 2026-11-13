// A guard that fails inside a loop, the rest of that call running in the
// resume twin, and the function then called again: the next call starts in
// the speculated code and leaves again only where a guard fails again.

class V { constructor(a) { this.a = a; this.b = a + 1; } }

function total(items, k) {
  let s = 0;
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    s += it.a * k;               // leaves at the first odd item
    s += it.b;
    if (s > 1e9) s -= 1e9;
  }
  return s;
}

const plain = [];
for (let i = 0; i < 64; i++) plain.push(new V(i));
const mixed = plain.slice();
mixed[40] = { b: 5, a: 6 };
const late = plain.slice();
late[63] = { a: 1, b: 2, c: 3 };

let sum = 0;
for (let r = 0; r < 3000; r++) {
  sum += total(plain, r & 7);
  if (r % 100 === 99) sum += total(mixed, r & 7);
  if (r % 250 === 249) sum += total(late, r & 7);
  sum %= 1000000007;
}
console.log(sum);
console.log(total(mixed, 2), total(plain, 2), total(late, 2), total(plain, 3));
