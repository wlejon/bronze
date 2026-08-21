// The for-of step that generated code now performs INLINE for a record the
// open classified as an array walk (codegen-llvm/llvm_iter.h), and everything
// that must keep taking the runtime's own step instead.
//
// The inline path is a copy of `stepFast`'s Array arm, so what this case pins
// is that the copy is exact — the hole rule, the length re-read per element,
// the shifted `head`, and the kinds it refuses. A case that only summed a dense
// array would pass with the guard inverted.

// --- dense ----------------------------------------------------------------
let s1 = 0;
for (const x of [1, 2, 3]) s1 = s1 + x;
console.log(s1);

// --- a HOLE iterates as undefined, and is not skipped (23.1.5.1 uses Get) --
const holed = [1, , 3];
const out2 = [];
for (const x of holed) out2.push(String(x));
console.log(out2.join(','));

// --- the LENGTH is re-read per element, so a push extends the walk ---------
const grow = [1, 2, 3];
const seen3 = [];
for (const x of grow) {
    seen3.push(x);
    if (x === 1) grow.push(99);
}
console.log(seen3.join(','));

// --- and a shrink ends it early -------------------------------------------
const shrink = [1, 2, 3, 4];
const seen4 = [];
for (const x of shrink) {
    seen4.push(x);
    if (x === 2) shrink.length = 2;
}
console.log(seen4.join(','));

// --- an element REPLACED ahead of the cursor is seen at its index ----------
const mutated = [1, 2, 3];
const seen5 = [];
for (const x of mutated) {
    seen5.push(x);
    if (x === 1) mutated[2] = 'replaced';
}
console.log(seen5.join(','));

// --- break ----------------------------------------------------------------
let first6 = 0;
for (const x of [7, 8, 9]) { first6 = x; break; }
console.log(first6);

// --- nested walks over ONE array need two records --------------------------
let acc7 = '';
const pair7 = [1, 2];
for (const x of pair7) for (const y of pair7) acc7 = acc7 + x + y + ' ';
console.log(acc7.trim());

// --- a second walk starts over ---------------------------------------------
const twice8 = [1, 2, 3];
let t8a = 0;
let t8b = 0;
for (const x of twice8) t8a = t8a + x;
for (const x of twice8) t8b = t8b + x;
console.log(t8a, t8b);

// --- empty --------------------------------------------------------------
let count9 = 0;
for (const x of []) count9 = count9 + 1;
console.log(count9);

// --- element IDENTITY, not a copy ------------------------------------------
const o10a = { v: 1 };
const o10b = { v: 2 };
let same10 = true;
for (const x of [o10a, o10b]) same10 = same10 && (x === o10a || x === o10b);
console.log(same10);

// --- a SHIFTED array, whose element block starts at a non-zero head --------
const shifted11 = [0, 1, 2, 3, 4];
shifted11.shift();
shifted11.shift();
let hs11 = '';
for (const x of shifted11) hs11 = hs11 + x;
console.log(hs11);

// --- long enough to have grown its element block more than once ------------
const big12 = [];
for (let i = 0; i < 1000; i = i + 1) big12.push(i);
let bs12 = 0;
for (const x of big12) bs12 = bs12 + x;
console.log(bs12);

// --- destructuring and spread take the same records ------------------------
const [p13, q13, ...rest13] = [1, 2, 3, 4];
console.log(p13, q13, rest13.join(','));
console.log([...[1, 2, 3]].join('-'));

// --- kinds the inline path REFUSES, each still answering ------------------
let ts14 = 0;
for (const x of new Float64Array([1.5, 2.5])) ts14 = ts14 + x;
console.log(ts14);

const cps15 = [];
for (const c of 'a\u{1F600}b') cps15.push(c.codePointAt(0));
console.log(cps15.join(','));

let ss16 = 0;
for (const x of new Set([1, 2, 3])) ss16 = ss16 + x;
console.log(ss16);

let ms17 = '';
for (const kv of new Map([['a', 1], ['b', 2]])) ms17 = ms17 + kv[0] + kv[1];
console.log(ms17);

function* gen18() { yield 1; yield 2; }
let gs18 = 0;
for (const x of gen18()) gs18 = gs18 + x;
console.log(gs18);

// A user-defined iterable: the protocol path, one call into JS per element.
const custom19 = {};
custom19[Symbol.iterator] = function () {
    let i = 0;
    return {
        next: function () {
            if (i < 3) { const v = i; i = i + 1; return { value: v, done: false }; }
            return { value: undefined, done: true };
        }
    };
};
let cs19 = 0;
for (const x of custom19) cs19 = cs19 + x;
console.log(cs19);

// --- an array SUBCLASS is still an array exotic object ---------------------
class Sub20 extends Array {}
const sub20 = new Sub20();
sub20.push(1, 2, 3);
let ss20 = 0;
for (const x of sub20) ss20 = ss20 + x;
console.log(ss20, sub20 instanceof Sub20, sub20.length);

// --- an exception thrown mid-walk leaves the loop ---------------------------
let caught21 = '';
try {
    for (const x of [1, 2, 3]) {
        if (x === 2) throw new Error('stop');
    }
} catch (e) {
    caught21 = e.message;
}
console.log(caught21);

// --- an array of arrays, destructured per element --------------------------
let acc22 = '';
for (const [a, b] of [[1, 2], [3, 4]]) acc22 = acc22 + a + b;
console.log(acc22);
