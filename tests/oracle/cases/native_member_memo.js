// The native-member memo (runtime/native_fn_memo.h): reading `m.get` off a
// collection answers from a (kind, key) table instead of walking a C member
// ladder to a fresh interning.
//
// Function IDENTITY is what a memo over function objects can get wrong, so it
// is what this case pins first and hardest: the memo must not merge two
// members, must not survive an own property shadowing it, and must not answer
// for a kind it was not filled for. Everything here is warmed in a loop,
// because a table that never fills passes each assertion on its first read.

// --- one member is ONE object ----------------------------------------------
const m = new Map([['a', 1]]);
console.log(m.get === m.get);
const g0 = m.get;
for (let i = 0; i < 300; i = i + 1) m.get('a');
console.log(m.get === g0);

// Two INSTANCES of one kind share the member, because it is the prototype's.
const m2 = new Map();
console.log(m2.get === m.get);

// --- two members of one kind are two objects -------------------------------
console.log(m.get === m.set, m.get === m.has);

// --- two KINDS do not share an entry ---------------------------------------
const s = new Set([1, 2]);
console.log(typeof s.has, typeof s.add, typeof m.has);
console.log(s.has(1), s.has(9), m.has('a'), m.has('z'));
// Their BEHAVIOUR is what an entry crossing kinds would break, and it is what
// is pinned: a Map's `has` takes a key, a Set's takes a value, and each must
// answer about its own collection after the memo has been warm for both.
//
// Their IDENTITY is deliberately not pinned. `Set.prototype.has` and
// `Map.prototype.has` share one C implementation (`mapHas`) and
// `bronze_function_singleton` interns on the code pointer, so bronze answers
// `m.has === s.has` as true where 24.2.3.7 and 24.1.3.7 are two distinct
// function objects. That is a standing divergence this chunk found and did not
// introduce — the memo is keyed on (kind, key) and merges nothing — and
// recording a wrong answer as an expectation is worse than leaving it unpinned.
for (let i = 0; i < 300; i = i + 1) { m.has('a'); s.has(1); }
console.log(m.has('a'), m.has(1), s.has(1), s.has('a'));

// --- an OWN property shadows the builtin, and the memo sits below it -------
const shadowed = new Map([['k', 'builtin']]);
for (let i = 0; i < 300; i = i + 1) shadowed.get('k');
shadowed.get = function () { return 'mine'; };
console.log(shadowed.get('k'));
delete shadowed.get;
console.log(typeof shadowed.get, shadowed.get('k'));

// --- a WeakMap, which is what three.js's renderer actually reads -----------
const wm = new WeakMap();
const key = {};
for (let i = 0; i < 300; i = i + 1) wm.set(key, i);
console.log(wm.get(key), wm.has(key), wm.has({}));
console.log(typeof wm.delete, wm.delete(key), wm.has(key));

const ws = new WeakSet();
ws.add(key);
console.log(ws.has(key), typeof ws.add);

// --- `size` is not a member the memo can hold: it is a number, computed ----
const sized = new Map([['a', 1], ['b', 2]]);
let z = 0;
for (let i = 0; i < 300; i = i + 1) z = sized.size;
console.log(z);
sized.set('c', 3);
console.log(sized.size);
sized.delete('a');
console.log(sized.size);

// --- one link above the ladder: Object.prototype ---------------------------
// Deliberately NOT memoized — a write to `Object.prototype.toString` replaces a
// slot without transitioning a shape or bumping an epoch, so the memo would
// have nothing to invalidate against.
console.log(typeof m.hasOwnProperty, m.hasOwnProperty('nothing'));

// --- the members still WORK after the warm-up, on fresh receivers ----------
let total = 0;
for (let i = 0; i < 300; i = i + 1) {
    const fresh = new Map();
    fresh.set('n', i);
    total = total + fresh.get('n');
}
console.log(total);

// --- and are callable when detached from the receiver ----------------------
const getFn = m.get;
console.log(getFn.call(m, 'a'));
