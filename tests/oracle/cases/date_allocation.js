// Dates under allocation pressure.
//
// Every case in this suite runs a second time under BRONZE_GC_STRESS=1, which
// collects on every allocation — and a Date is the first bronze object that is
// a plain object with INTERNAL SLOTS whose methods run user-visible conversions
// (`@@toPrimitive` reaches `toString`, which allocates a string, which under
// stress moves the receiver). A rooting mistake there is not a crash but an
// intermittent wrong answer, so this case allocates thousands of them and
// checks a total that only comes out right if every single one survived.

// ---- a long-lived Date across many collections ------------------------------
const anchor = new Date(Date.UTC(2000, 0, 1));
let junk = 0;
for (let i = 0; i < 5000; i++) {
  const scratch = new Date(i * 86400000);
  junk += scratch.getUTCDate();
}
console.log(anchor.toISOString(), anchor.getTime());
console.log(junk);

// ---- the sum of a run of instants -------------------------------------------
// Built through the field constructor and read back through the getters, so a
// Date that moved between the two would show up here as a wrong total.
let total = 0;
let days = 0;
for (let i = 0; i < 2000; i++) {
  const d = new Date(Date.UTC(1970, 0, 1 + i));
  total += d.getTime();
  days += d.getUTCDay();
}
console.log(total, days);

// ---- conversions in the loop ------------------------------------------------
// Each iteration builds a Date, runs it through ToPrimitive twice (once with
// hint string, once with hint number) and through toISOString, every one of
// which allocates while the receiver is live.
let sameCount = 0;
let charCount = 0;
for (let i = 0; i < 1000; i++) {
  const d = new Date(Date.UTC(2020, 0, 1) + i * 1000);
  const text = "" + d;
  if (text === d.toString()) sameCount++;
  charCount += d.toISOString().length;
  if (d - new Date(Date.UTC(2020, 0, 1)) !== i * 1000) sameCount = -1;
}
console.log(sameCount, charCount);

// ---- Dates held in containers across collections ----------------------------
const kept = [];
for (let i = 0; i < 500; i++) {
  kept.push(new Date(Date.UTC(2020, 0, 1) + i * 86400000));
}
let sum = 0;
for (const d of kept) {
  sum += d.getUTCDate() + d.getUTCMonth();
}
console.log(kept.length, sum);
console.log(kept[0].toISOString(), kept[499].toISOString());

const byKey = new Map();
for (let i = 0; i < 300; i++) {
  byKey.set(i, new Date(Date.UTC(1990, 0, 1) + i * 3600000));
}
console.log(byKey.size, byKey.get(0).toISOString(), byKey.get(299).toISOString());

// ---- and the JSON round trip, which allocates the most ----------------------
const payload = JSON.stringify(kept.slice(0, 3));
console.log(payload);
console.log(JSON.parse(payload).length, JSON.parse(payload)[2]);
