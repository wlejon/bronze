// Adds speculated in one order, then receivers arriving with the keys in
// another order, with some keys already present, and frozen.
function main() {
  function init(o) { o.x = 1; o.y = 2; o.z = 3; return o; }
  let s = 0; const out = [];
  for (let i = 0; i < 30000; i++) {
    let o;
    if (i < 15000) o = {};
    else if (i % 4 === 0) o = { y: 0 };
    else if (i % 4 === 1) o = { z: 9, x: 8 };
    else if (i % 4 === 2) o = Object.preventExtensions({ q: 1 });
    else o = Object.create({ base: 1 });
    try { init(o); } catch (e) { out.push(e.constructor.name); }
    s += (o.x | 0) + (o.y | 0) + (o.z | 0);
    if (i % 7001 === 0) out.push(Object.keys(o).join(""));
  }
  console.log(s, out.join(" "));
}
main();
