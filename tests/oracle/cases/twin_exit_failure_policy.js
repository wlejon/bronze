// The failure policy of resume twins (il2mir/il_speculation.h): a function
// whose guards fail on most calls ends up called through its generic entry,
// one that fails now and then is probed and trusted again, and one whose
// failures stop is still right after having been given up. Every phase runs
// long enough to pass the exit limit and a whole probe, and the answers are
// the generic code's throughout.

class V { constructor(a) { this.a = a; this.b = a + 1; } }

function total(items, k) {
  let s = 0;
  for (let i = 0; i < items.length; i++) {
    const it = items[i];
    s += it.a * k;
    s += it.b;
    if (s > 1e9) s -= 1e9;
  }
  return s;
}

function first(items) {
  const it = items[0];
  return it.a + it.b * 2;
}

const plain = [];
for (let i = 0; i < 16; i++) plain.push(new V(i));
const odd = plain.slice();
odd[0] = { b: 5, a: 6 };
const odder = plain.slice();
odder[3] = { a: 1, b: 2, c: 3 };

let sum = 0;
function run(n, every) {
  for (let r = 0; r < n; r++) {
    const bad = every > 0 && r % every === 0;
    sum += total(bad ? (r & 1 ? odd : odder) : plain, r & 7);
    sum += first(bad ? odd : plain);
    sum %= 1000000007;
  }
  console.log(sum);
}

run(20000, 0);     // warm, no failures
run(40000, 64);    // rare failures: past the exit limit, probed, trusted again
run(20000, 0);
run(20000, 1);     // every call fails: generic from here on
run(20000, 0);     // and still right once the failures stop
run(20000, 3);
console.log(total(odd, 2), total(plain, 2), total(odder, 2), first(odd), first(plain));
