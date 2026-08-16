// A BigInt is a heap object with a variable-length payload the collector must
// COPY and must not SCAN — the string's layout, with limbs where a string has
// code units. This case is the proof: it allocates far more BigInts than the
// semispace holds, keeps some of them alive through every kind of edge the
// collector follows, and checks the values afterwards. Under
// BRONZE_GC_STRESS=1 every allocation collects, so each line below moves the
// whole live set at least once.
function fact(n) {
  let r = 1n;
  for (let i = 2n; i <= n; i++) r = r * i;
  return r;
}

// Held in an array: the elements block is traced, so a moved BigInt must have
// its forwarding address written back through it.
const facts = [];
for (let i = 0; i < 60; i++) facts.push(fact(BigInt(i)));
console.log(facts[0], facts[1], facts[20]);
console.log(facts[50] / facts[49] === 50n, facts.length);

// Held in object properties, which move through a different slot kind.
const objs = [];
for (let i = 0; i < 200; i++) {
  objs.push({ id: i, big: BigInt(i) * 1000000007n, pad: "x".repeat(i % 11) });
}
let sum = 0n;
for (const o of objs) sum += o.big;
console.log(sum, sum === 1000000007n * 19900n);

// Held as Map keys and Set members, where identity is SameValueZero over the
// VALUE — so a collection that moved a key must still find it.
const m = new Map();
const s = new Set();
for (let i = 0; i < 200; i++) { m.set(BigInt(i) * 7n, i); s.add(BigInt(i) % 5n); }
console.log(m.size, s.size, m.get(70n), m.get(1393n), m.has(1n));

// Held in a closure's captured environment.
const closures = [];
for (let i = 0; i < 50; i++) { const v = 2n ** BigInt(i); closures.push(() => v); }
console.log(closures[0](), closures[10](), closures[49]());
console.log(closures[49]() === 2n ** 49n);

// Held across a collection that happens INSIDE the operator: `a * b`
// allocates its result, and both operands must survive that allocation.
let acc = 1n;
for (let i = 0; i < 300; i++) acc = (acc * 1000003n + BigInt(i)) % (2n ** 128n);
console.log(acc);

// And through the exception path, where the value is in the pending cell.
try { throw 123456789012345678901234567890n; } catch (e) { console.log(e, typeof e); }
console.log(facts[20], objs[199].big, [...s].sort((a, b) => (a < b ? -1 : 1)));
