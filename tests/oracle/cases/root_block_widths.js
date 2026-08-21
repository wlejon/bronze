// Root blocks at the widths runtime/root_slots.h's arithmetic can tell apart:
// below the inline buffer, exactly at it, one past it, and far past it — with
// the collector free to run through the middle of every one of them.
//
// What is pinned is not the widths. It is that an argument a builtin received
// is still the SAME value after that builtin allocates, after a callback it
// ran allocated, and after a collection moved everything under both. A block
// that stopped rooting one of its slots answers here with a stale from-space
// address read as a Value, and under BRONZE_GC_STRESS=1 it answers that way on
// the first try rather than the thousandth.
//
// Every argument below is a FRESH object that nothing else names, so the block
// is genuinely the only root: a spread argument list is built, handed over,
// and dropped, and if the callee's copy is not a root there is nothing keeping
// it alive at all.

function objs(n) {
    const a = [];
    for (let i = 0; i < n; i = i + 1) a.push({ v: i });
    return a;
}

function sum(a) {
    let s = 0;
    for (let i = 0; i < a.length; i = i + 1) s = s + a[i].v;
    return s;
}

// --- a builtin that ALLOCATES before it reads its arguments -----------------
// Array.of creates the array first and copies the arguments into it after,
// which is exactly the window `argv` stops being rooted in.
for (const n of [0, 1, 8, 9, 64]) {
    const made = Array.of(...objs(n));
    console.log('of', n, made.length, sum(made));
}

// The argument whose ONLY reference is the block. Nothing above names these,
// so an unrooted slot is a collected object, not merely a moved one.
console.log('only', Array.of({ v: 7 }, { v: 8 }, { v: 9 }).map(function (o) { return o.v; }).join('/'));

// --- two blocks live at once ------------------------------------------------
// A bound function's stored arguments are one block and the call's are
// another, and the callee sees them spliced into a third.
function tag() {
    let s = 0;
    for (let i = 0; i < arguments.length; i = i + 1) s = s + arguments[i].v;
    return [arguments.length, s];
}

for (const n of [0, 1, 8, 9, 64]) {
    const bound = tag.bind(null, ...objs(n));
    const r = bound(...objs(n));
    console.log('bound', n, r[0], r[1]);
}

// apply builds the block out of an array rather than out of a call site.
for (const n of [0, 1, 8, 9, 64]) {
    const r = tag.apply(null, objs(n));
    console.log('apply', n, r[0], r[1]);
}

// --- a callback that allocates on every element -----------------------------
// The builtin holds its own arguments across N re-entries into JS, each of
// which can collect.
const src = objs(40);
const mapped = src.map(function (e, i, whole) {
    const junk = { a: e.v, b: i, c: whole.length };
    return junk.a + junk.b + junk.c;
});
console.log('map', mapped.length, mapped[0], mapped[39],
            mapped.reduce(function (a, b) { return a + b; }, 0));

// --- a builtin re-entered through JS while its own block is live ------------
// Each level's `concat` arguments are still rooted while the level below runs
// its own `concat` and its own `map`, so seven blocks are open at the bottom.
function deep(level) {
    if (level === 0) return [1];
    return [level].concat(deep(level - 1).map(function (x) { return x + level; }));
}
const d = deep(6);
console.log('deep', d.length, d.join(','));

// --- a Proxy apply trap: the trap's block and the target's, together --------
const proxied = new Proxy(tag, {
    apply(target, thisArg, list) {
        return Reflect.apply(target, thisArg, list.concat(list));
    }
});
for (const n of [0, 1, 9]) {
    const r = proxied(...objs(n));
    console.log('proxy', n, r[0], r[1]);
}

// --- the RECEIVER is a root of the block too --------------------------------
// `Array.prototype.concat` allocates the result array first and only then
// copies the receiver's elements and its arguments' into it, so the receiver
// is live across an allocation exactly as the arguments are.
const base = [{ v: 100 }];
console.log('recv', sum(base.concat(objs(9))), base.length);

// --- the seam changes where the slots live and nothing else -----------------
// Re-run the widest of the shapes above after a deliberate churn, so the run
// under BRONZE_NO_INLINE_ROOTS=1 (every block on the C heap) and the run
// without it (the first eight slots inline) have to agree byte for byte.
let churn = null;
for (let i = 0; i < 200; i = i + 1) churn = { i: i, prev: churn };
console.log('churn', churn.i, churn.prev.i);
console.log('again', sum(Array.of(...objs(64))), tag.apply(null, objs(9))[1]);
