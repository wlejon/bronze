// Array.prototype.sort's comparator, called through the runtime's DIRECT
// call-out (runtime/call_out.h) instead of the generic dynamic-call path.
//
// The call-out resolves the comparator once and calls its code pointer per
// comparison, and it moves the NewTargetScope out of the per-comparison loop
// and around the whole sort. So what has to be pinned is: every callee shape
// the binding may refuse still sorts identically, a comparator that mutates or
// throws behaves as it did, and new.target reads `undefined` in every plain
// call the sort makes.

// --- 1. the ordinary case: a plain comparator ------------------------------
const nums = [5, 3, 9, 1, 7, 3, 8, 2, 6, 4];
console.log(nums.slice().sort((a, b) => a - b).join(','));
console.log(nums.slice().sort((a, b) => b - a).join(','));

// --- 2. STABILITY, which the merge sort owes and the call-out must not move -
const recs = [
    { k: 1, tag: 'a' }, { k: 0, tag: 'b' }, { k: 1, tag: 'c' },
    { k: 0, tag: 'd' }, { k: 1, tag: 'e' }, { k: 0, tag: 'f' }
];
console.log(recs.slice().sort((x, y) => x.k - y.k).map(r => r.tag).join(''));

// --- 3. comparator ARITIES the binding treats differently ------------------
// Two formals is what the sort passes. Fewer is still reached directly; MORE
// needs undefined padding, which the binding refuses — and a comparator that
// reads a third parameter must see undefined either way.
function twoArg(a, b) { return a - b; }
// A one-formal comparator is not a consistent ordering, so only a case with a
// SINGLE comparison has an answer the language pins: `compare(3, 1)` is
// negative, so 3 keeps its place.
function oneArg(a) { return a - 5; }
function threeArg(a, b, c) { return (c === undefined ? 1 : -1) * (a - b); }
console.log([3, 1, 2].sort(twoArg).join(','));
console.log([3, 1].sort(oneArg).join(','));
console.log([3, 1, 2].sort(threeArg).join(','));

// --- 4. a BOUND comparator -------------------------------------------------
function cmpBy(key, a, b) { return a[key] - b[key]; }
const byV = cmpBy.bind(null, 'v');
console.log([{ v: 3 }, { v: 1 }, { v: 2 }].sort(byV).map(o => o.v).join(','));

// --- 5. a comparator that is not callable ----------------------------------
// 23.1.3.30 step 1: not undefined and not callable is a TypeError, thrown
// BEFORE anything is read — so the binding is never attempted and the array is
// untouched.
//
// A callable PROXY belongs in this section and is not here: bronze refuses one
// as a comparator, where 23.1.3.30 asks IsCallable and a proxy over a function
// answers true. That is a standing divergence this chunk found and did not
// introduce — `DirectCallee::bind` refuses a proxy for its own reasons, and
// pinning bronze's refusal here would record a wrong answer as an expectation.
const notCallable = [4, 2, 6, 1];
let typeErr = false;
try {
    notCallable.sort(42);
} catch (e) {
    typeErr = e instanceof TypeError;
}
console.log(typeErr, notCallable.join(','));

// --- 6. a comparator that MUTATES the array mid-sort -----------------------
// The sort took its own list of the elements first, so the pushes below cannot
// change what is being sorted; the receiver keeps them past the sorted items.
const mut = [3, 1, 2];
const mutated = mut.sort(function (a, b) { mut.push(99); return a - b; });
console.log(mutated === mut, mut.length >= 3, mut.slice(0, 3).join(','));

// --- 7. a comparator that THROWS -------------------------------------------
// The throw ends the whole sort, and the receiver is left as it was: the
// write-back never ran.
const thrower = [4, 3, 2, 1];
let caught = '';
try {
    thrower.sort(function () { throw new Error('cmp'); });
} catch (e) {
    caught = e.message;
}
console.log(caught, thrower.join(','));

// --- 8. a comparator returning a non-number --------------------------------
// 23.1.3.30.2 step 4: ToNumber of the answer, and NaN reads as "equal".
console.log([3, 1, 2].sort(() => NaN).join(','));
console.log([3, 1, 2].sort(() => undefined).join(','));
console.log(['b', 'a', 'c'].sort(() => '0').join(','));

// --- 9. an answer whose ToNumber runs USER CODE ----------------------------
// The object's valueOf is a plain call made by the sort, between two
// comparator calls — which is exactly the window the hoisted NewTargetScope
// covers.
let valueOfCalls = 0;
function boxed(n) { return { valueOf() { valueOfCalls = valueOfCalls + 1; return n; } }; }
console.log([3, 1, 2].sort((a, b) => boxed(a - b)).join(','), valueOfCalls > 0);

// --- 10. new.target inside every plain call the sort makes -----------------
// 13.3.12: a function CALLED rather than constructed sees `undefined`, and
// that holds for the comparator and for the valueOf the sort reaches through
// ToNumber — including when the sort itself runs inside a constructor.
let seenInCmp = 'unset';
let seenInValueOf = 'unset';
function cmpTarget(a, b) {
    seenInCmp = new.target === undefined ? 'undefined' : 'defined';
    return {
        valueOf() {
            seenInValueOf = new.target === undefined ? 'undefined' : 'defined';
            return a - b;
        }
    };
}
class Sorter {
    constructor() {
        this.out = [3, 1, 2].sort(cmpTarget).join(',');
    }
}
const sorter = new Sorter();
console.log(sorter.out, seenInCmp, seenInValueOf);

// --- 11. no comparator at all: the string comparison path ------------------
// The binding is never made, and 23.1.3.30.2 steps 5-9 compare code units.
console.log([10, 9, 1, 100].sort().join(','));
console.log(['Z', 'a', 'B'].sort().join(','));

// --- 12. undefineds and holes sort to the end ------------------------------
const sparse = [3, undefined, 1, , 2];
console.log(sparse.sort((a, b) => a - b).length, sparse[0], sparse[1], sparse[2],
    sparse[3] === undefined);

// --- 13. a big enough sort to exercise every merge width -------------------
const big = [];
for (let i = 0; i < 200; i = i + 1) big.push((i * 37) % 199);
big.sort((a, b) => a - b);
console.log(big[0], big[99], big[199], big.length);
