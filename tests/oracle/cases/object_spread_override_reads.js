// A literal key a later spread overrides reads back as the SPREAD's value.
//
// `{ from: -1, ...p }` defines `from` and then CopyDataProperties (ECMA-262
// 7.3.25) redefines it from `p`, so `P.from` is p's value. A read that answers
// the literal's -1 has forwarded the earlier store across the spread, as if the
// spread wrote nothing. The computed-key read and Object.assign are the
// control: they answer the same question by another road.
//
// Every shape of the override is here, because each puts the stale key in a
// different place: one key or several, keys the spread adds in another order
// (an existing key keeps its POSITION and takes the new value), a literal key
// AFTER the spread winning over it, two spreads in one literal, a spread of a
// spread, a getter in the source (its answer is the value, read once), and a
// read, a write and a read again of the overridden key.
//
// No reflection (`Object.defineProperty` and friends) anywhere in this file: a
// function that touches it takes its keys out of the forwarding pass, which
// would hide exactly the read this case is about.

const lit = (p) => { const P = { from: -1, ...p }; return P.from; };
const asg = (p) => { const P = Object.assign({ from: -1 }, p); return P.from; };
const computed = (p) => { const P = { from: -1, ...p }; const k = "from"; return P[k]; };
console.log(lit({ from: 5 }), asg({ from: 5 }), computed({ from: 5 }));
console.log(lit({}), lit({ other: 1 }), lit(null), lit(undefined));

const many = (p) => {
  const P = { a: 1, b: 2, c: 3, ...p };
  return [P.a, P.b, P.c, Object.keys(P).join("")].join(" ");
};
console.log(many({ c: 30, a: 10 }));

const order = (p) => {
  const P = { x: 1, y: 2, ...p };
  return [P.x, P.y, P.z, Object.keys(P).join("")].join(" ");
};
console.log(order({ z: 3, y: 20, x: 10 }));

const after = (p) => { const P = { ...p, a: 9 }; return [P.a, P.b].join(" "); };
console.log(after({ a: 1, b: 2 }));

const two = (p, q) => {
  const P = { a: 1, ...p, b: 2, ...q };
  return [P.a, P.b, P.c, Object.keys(P).join("")].join(" ");
};
console.log(two({ a: 10, b: 20 }, { b: 200, c: 300 }));
console.log(two({ a: 10, b: 20 }, {}));

const inner = (p) => { const P = { a: 1, b: 1, ...{ b: 2, ...p } }; return [P.a, P.b].join(" "); };
console.log(inner({ a: 5, b: 6 }), inner({}));

let reads = 0;
const viaGetter = (p) => { const P = { a: 1, ...p }; return [P.a, P.a].join(" "); };
console.log(viaGetter({ get a() { reads++; return 42; } }), reads);

const rewrite = (p) => {
  const P = { from: -1, ...p };
  const first = P.from;
  P.from = first + 1;
  return [first, P.from].join(" ");
};
console.log(rewrite({ from: 5 }));

let sum = 0;
for (let i = 0; i < 2000; i++) sum += lit({ from: i });
console.log(sum);
