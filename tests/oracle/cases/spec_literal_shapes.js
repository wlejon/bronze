// One literal site whose values change kind partway, objects from it
// reshaped after they are made, and a second site with the keys reordered.
function main() {
  function make(i, v) { return { a: v, b: i, c: [v, i] }; }
  function other(i) { return { b: i, a: -i }; }
  let acc = 0;
  const kept = [];
  for (let i = 0; i < 30000; i++) {
    const v = i < 15000 ? i : (i % 3 === 0 ? "s" + i : i % 3 === 1 ? { n: i } : 1.5);
    const o = make(i, v);
    if (i % 5000 === 7) kept.push(o);
    acc += o.b + (typeof o.a === "number" ? o.a : 1) + o.c.length;
    const p = other(i);
    acc += p.a + p.b;
  }
  kept[1].z = 9; delete kept[2].a; Object.freeze(kept[3]);
  kept[3].a = 100;
  for (const k of kept) console.log(JSON.stringify(k), Object.keys(k).join(","));
  console.log(acc, JSON.stringify(other(3)), Object.keys(other(4)).join(","));
}
main();
