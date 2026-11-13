// References held across a guard that fails: strings, arrays and objects
// made before the failing read are in the state handed to the resume twin,
// which allocates enough afterwards for collections to run (and under
// BRONZE_GC_STRESS, at every allocation) while it still reads them.

class Item { constructor(i) { this.name = 'item' + i; this.tags = [i, i + 1]; this.next = null; } }

function build(prev, cfg, i) {
  const it = new Item(i);        // fresh references, live to the end
  const label = it.name + ':' + i;
  const pair = [prev, it];
  it.next = prev;
  const w = cfg.width;           // the failing read
  const filler = [];
  for (let j = 0; j < 8; j++) filler.push({ j, s: label + j });
  return { it, label, n: pair.length + filler.length + w, first: filler[0].s, tag: it.tags[1] };
}

function run(n, oddCfg) {
  const cfg = { width: 2 };
  let prev = null;
  let s = 0;
  let last = null;
  for (let i = 0; i < n; i++) {
    last = build(prev, i % 61 === 60 ? oddCfg : cfg, i);
    prev = last.it;
    s += last.n + last.tag + last.label.length + last.first.length;
    if (i % 512 === 0) prev = null;
  }
  let chain = 0;
  for (let p = prev; p !== null; p = p.next) chain++;
  return s + ':' + chain + ':' + last.label + ':' + last.first;
}

console.log(run(6000, { depth: 1, width: 3 }));
console.log(run(6000, { width: 4, depth: 1 }));
console.log(run(6000, Object.freeze({ a: 0, width: 5 })));
