// A property read twice with user code between the reads answers what the
// property holds at the SECOND read.
//
// None of the instructions between the reads below is a store or a call in the
// source, and every one of them runs user code all the same: an object spread
// runs the source's getters, an array spread and for-of run the iterator,
// object rest runs getters, and a read of an accessor key runs the getter. Each
// of them here writes the property read on either side of it, so a re-read
// forwarded from the first read — or hoisted out of the loop — prints the old
// value. (The implicit hooks — `valueOf` under a boxed `+` and friends — are
// the blocked half: cases/blocked/prop_reread_across_to_primitive.js.)

class Box { constructor() { this.v = 1; } }
const shared = new Box();
const reset = () => { shared.v = 1; };

const viaSpread = (src) => { const a = shared.v; const t = { ...src }; return [a, shared.v, t.k].join(" "); };
reset(); console.log("spread", viaSpread({ get k() { shared.v = 7; return 3; } }));

const viaArraySpread = (it) => { const a = shared.v; const arr = [...it]; return [a, shared.v, arr.length].join(" "); };
reset(); console.log("array spread", viaArraySpread({ *[Symbol.iterator]() { shared.v = 4; yield 1; } }));

const viaRest = (src) => { const a = shared.v; const { x, ...rest } = src; return [a, shared.v, x, Object.keys(rest).join("")].join(" "); };
reset(); console.log("rest", viaRest({ x: 1, get y() { shared.v = 6; return 2; } }));

const viaForOf = (it) => { const a = shared.v; let n = 0; for (const _ of it) n++; return [a, shared.v, n].join(" "); };
reset(); console.log("for-of", viaForOf({ *[Symbol.iterator]() { shared.v = 5; yield 1; } }));

const trap = { get poke() { shared.v = 3; return 0; } };
const viaGetter = () => { const a = shared.v; const p = trap.poke; return [a, shared.v, p].join(" "); };
reset(); console.log("getter", viaGetter());

// The same writes inside a loop: the read may not move out of it.
const loopSpread = (src) => { let s = ""; for (let i = 0; i < 3; i++) { s += shared.v; const t = { ...src }; } return s; };
reset(); console.log("loop spread", loopSpread({ get k() { shared.v++; return 0; } }));

const loopArraySpread = (it) => { let s = ""; for (let i = 0; i < 3; i++) { s += shared.v; const arr = [...it]; } return s; };
reset(); console.log("loop array spread", loopArraySpread({ *[Symbol.iterator]() { shared.v++; } }));

const counter = { get tick() { shared.v++; return 0; } };
const loopTick = () => { let s = ""; for (let i = 0; i < 3; i++) { s += shared.v; const p = counter.tick; } return s; };
reset(); console.log("loop getter", loopTick());
