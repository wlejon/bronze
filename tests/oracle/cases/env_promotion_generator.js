// Stage R3, observability class (c): a suspension is a region boundary.
//
// A generator body is compiled as a resume function that is entered from the
// top and left at every `yield`, so a slot promoted inside it is written back
// before control leaves — which is what lets `get` and `add`, ordinary closures
// over the same record, see and change `n` BETWEEN two `next()` calls.
//
// The middle two numbers are the whole point: the first says the record is
// current at the suspension, the second says the generator picked up what was
// written into it while it was suspended.
function makeCounter() {
  let n = 0;

  function* pump(times) {
    for (let i = 0; i < times; i++) {
      n = n + 1;
      yield n;
    }
  }

  return {
    pump: pump,
    get: function () { return n; },
    add: function (k) { n = n + k; }
  };
}

const c = makeCounter();
const it = c.pump(3);
const out = [];
out.push(it.next().value);
out.push(c.get());
c.add(10);
out.push(it.next().value);
out.push(c.get());
out.push(it.next().value);
console.log(out.join(","));
