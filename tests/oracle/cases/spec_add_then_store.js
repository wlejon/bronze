// One assignment site that adds its key to a fresh object the first time it
// sees it and stores to the same key on every later call (a renderer's
// per-object properties bag, as three.js's `sourceProperties.__version`).
// The harvest's one way for the site names the shape after the add, which
// both kinds of receiver go through; the store must take the slot and the
// add must take the transition, with no exit for either.
const bags = new Map();
function propsOf(key) {
  let p = bags.get(key);
  if (p === undefined) { p = {}; bags.set(key, p); }
  return p;
}
function upload(key, version) {
  const p = propsOf(key);
  p.__version = version;
  return p.__version;
}
function main() {
  let s = 0;
  for (let frame = 0; frame < 400000; frame++) {
    s += upload(frame % 7, frame);
    if (frame % 50000 === 0) s += upload("k" + frame, -frame);
  }
  const p = propsOf(3);
  console.log(s, bags.size, p.__version, Object.keys(p).join(","));
}
main();
