// Many generators and async functions suspended at once, each holding heap
// objects, strings and closures only in its frame, while the program
// allocates between their steps: every value a frame holds across a
// suspension has to come back intact however many collections ran meanwhile
// (the oracle runs this under gc-stress too).

function* holder(id) {
  const obj = { id, tags: ["t" + id], nested: { depth: id % 3 } };
  const text = "gen-" + id;
  const fn = (k) => obj.id * k + obj.tags.length;
  let acc = [];
  for (let step = 0; step < 4; step++) {
    acc.push(step * id);
    const sent = yield step;
    if (sent) obj.tags.push(sent);
  }
  return [text, obj.tags.join("+"), obj.nested.depth, fn(2), acc.join("/")].join(" ");
}

const gens = [];
for (let i = 0; i < 40; i++) gens.push(holder(i));

let junk = [];
const results = [];
for (let round = 0; round < 5; round++) {
  for (let i = 0; i < gens.length; i++) {
    for (let j = 0; j < 20; j++) junk.push({ round, i, j, s: "x" + j });
    if (junk.length > 2000) junk = [];
    const r = gens[i].next(round === 2 && i % 5 === 0 ? "r" + i : undefined);
    if (r.done) results.push(r.value);
  }
}
console.log(results.length);
console.log(results[0]);
console.log(results[5]);
console.log(results[39]);

async function asyncHolder(id) {
  const box = { id, items: [] };
  for (let i = 0; i < 3; i++) {
    await null;
    box.items.push({ v: id * 10 + i, label: "L" + i });
  }
  return box.items.map((it) => it.label + ":" + it.v).join(",");
}

const pending = [];
for (let i = 0; i < 30; i++) pending.push(asyncHolder(i));
Promise.all(pending).then((all) => {
  console.log(all.length, all[0], all[29]);
});
