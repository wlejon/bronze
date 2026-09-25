// Values held only by compiled frames (SSA values, block params, spilled
// registers, the per-function argv block) survive collections that move them.
// Every shape of call that stages arguments through memory is exercised with
// objects allocated right before the call and read right after it.

function box(n) {
  return { n: n, tag: "b" + n };
}

function sum20(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9,
               a10, a11, a12, a13, a14, a15, a16, a17, a18, a19) {
  return a0.n + a1.n + a2.n + a3.n + a4.n + a5.n + a6.n + a7.n + a8.n + a9.n +
         a10.n + a11.n + a12.n + a13.n + a14.n + a15.n + a16.n + a17.n + a18.n + a19.n;
}

function Wide(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9,
              a10, a11, a12, a13, a14, a15, a16, a17, a18) {
  this.total = a0.n + a9.n + a18.n;
  this.last = a18.tag;
}

class Acc {
  constructor(seed) { this.items = [box(seed)]; }
  add(x, y, z) {
    this.items.push(x, box(y.n + z.n));
    return this.items.length;
  }
  total() {
    let t = 0;
    for (const it of this.items) t += it.n;
    return t;
  }
}

function countArgs() {
  let t = 0;
  for (let i = 0; i < arguments.length; i++) {
    const tmp = box(i);
    t += arguments[i].n + tmp.n;
  }
  return t;
}

function rest(first, ...others) {
  const extra = box(1000);
  let t = first.n + extra.n;
  for (const o of others) t += o.n;
  return t;
}

function makeCounter(start) {
  let state = box(start);
  return function (step) {
    const next = box(state.n + step.n);
    state = next;
    return state.n;
  };
}

function depth(n, carried) {
  if (n === 0) return carried.n;
  const mine = box(n);
  const below = depth(n - 1, box(carried.n + 1));
  return below + mine.n;
}

function run(rounds) {
  let wideSum = 0;
  let ctorSum = 0;
  let methodSum = 0;
  let argSum = 0;
  let restSum = 0;
  let closureSum = 0;
  let recSum = 0;
  let tags = "";
  const acc = new Acc(1);
  const counter = makeCounter(0);
  const dyn = [sum20][0];
  for (let r = 0; r < rounds; r++) {
    const a = box(r);
    const b = box(r + 1);
    const c = box(r + 2);
    wideSum += dyn(a, b, c, box(3), box(4), box(5), box(6), box(7), box(8), box(9),
                   box(10), box(11), box(12), box(13), box(14), box(15), box(16),
                   box(17), box(18), box(19));
    const w = new Wide(a, b, c, box(3), box(4), box(5), box(6), box(7), box(8), box(9),
                       box(10), box(11), box(12), box(13), box(14), box(15), box(16),
                       box(17), box(18));
    ctorSum += w.total;
    methodSum += acc.add(box(r), a, c);
    argSum += countArgs(a, b, c, box(r * 2));
    restSum += rest(a, b, box(7), c);
    closureSum += counter(box(1));
    recSum += depth(8, a);
    if (r % 100 === 0) tags += w.last + "," + a.tag + ";";
    // The originals are read again after every call above allocated.
    if (a.n !== r || b.n !== r + 1 || c.tag !== "b" + (r + 2)) {
      console.log("corrupt at round " + r);
      return;
    }
  }
  console.log("wide " + wideSum);
  console.log("ctor " + ctorSum);
  console.log("method " + methodSum);
  console.log("acc " + acc.total());
  console.log("arguments " + argSum);
  console.log("rest " + restSum);
  console.log("closure " + closureSum);
  console.log("recursion " + recSum);
  console.log(tags);
}

run(300);
