// Arrays (a header and its elements, two objects) and closure environments
// made inline, kept across calls that collect, captured, nested, and literal
// runs whose object nothing reads again next to ones that are read, stored
// and returned. Their elements and captured values must survive every
// collection the allocations trigger.

function churn(n) {
  let last = null;
  for (let i = 0; i < n; i++) last = [i, { i: i }];
  return last[1].i;
}

function pairHeld(a) {
  const xs = [a, { v: a + 1 }, [a + 2]];
  churn(100);
  return xs[0] + xs[1].v + xs[2][0] + xs.length;
}

function counter(start) {
  let n = start;
  return {
    inc() {
      n++;
      return n;
    },
    get() {
      return n;
    },
  };
}

function nestedEnvs(a) {
  const outer = a + 1;
  function mid(b) {
    const inner = b * 2;
    return function (c) {
      return outer + inner + c;
    };
  }
  const f = mid(a);
  churn(50);
  return f(3);
}

function deadLiteral(a, b) {
  const p = { x: a, y: b, z: a - b };
  return p.x * 2 + p.y + p.z;
}

function liveLiteral(a) {
  const p = { x: a, y: [a, a], z: { w: a } };
  churn(20);
  return p;
}

function bigLiteral(a) {
  const p = { a: a, b: a + 1, c: a + 2, d: a + 3, e: a + 4, f: a + 5, g: a + 6 };
  return p.a + p.b + p.c + p.d + p.e + p.f + p.g;
}

function mixedValues(a) {
  const p = { n: a, s: "s" + (a % 10), o: null, u: undefined, b: a % 2 === 0, d: a / 4 };
  return [p.n, p.s, p.o, p.u, p.b, p.d].join(",");
}

const live = [];
let total = 0;
const c = counter(10);
for (let i = 0; i < 4000; i++) {
  total += pairHeld(i);
  total += nestedEnvs(i);
  total += deadLiteral(i, 3);
  total += bigLiteral(i);
  c.inc();
  if (i % 7 === 0) live.push(liveLiteral(i));
  if (i % 1000 === 0) console.log(mixedValues(i), mixedValues(i + 1));
}
let check = 0;
for (const p of live) check += p.x + p.y[1] + p.z.w;
console.log(total, c.get(), live.length, check);
console.log(JSON.stringify(live[3]));
