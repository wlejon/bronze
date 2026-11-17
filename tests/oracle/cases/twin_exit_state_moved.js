// A leave's state holding an object the collector moved. `drive` reads the
// module's `caught` set once, before its loop, and uses it only in the catch's
// `add`, which training never reaches (run.py's switch mode: it throws, but
// with failAt 5): the optimistic copy cuts the add to its one leave, so the
// set is live through the loop as that exit's state alone. The set is young
// when the loop starts, and the loop allocates enough for minor collections
// to move it; a throw then leaves, and the twin must add to the set where it
// is now. The counter steps by 0.5 so that it is a double, and the loop has
// no other exit reading the set. x64's guard exits read such a value from
// where it lives (x64_isel_guard_state.cpp): they once read the untagged word
// the tagged value was cast from, which no stack map names, so the twin got
// the set's old address.
const TRAINING = Math.random() < 0.5;
const caught = new Set();

class Box { constructor(v) { this.v = v; this.w = [v, v + 1]; } }

function work(i, failAt) {
  const b = new Box(i);
  if (i === failAt) throw new Error('at ' + i);
  return b.w[1] - b.v;
}

function drive(n, failAt) {
  let s = 0;
  for (let i = 0; i < n; i += 0.5) {
    try {
      s += work(i, failAt);
    } catch (e) {
      if (failAt > 5) caught.add(e.message);
    }
  }
  return s;
}

if (TRAINING) {
  console.error('TRAINING');
  console.log(drive(400000, 5), caught.size);
} else {
  console.log(drive(400000, 399999), caught.size);
  console.log(drive(400000, 200000), caught.size, [...caught].join(','));
}
