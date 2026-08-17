// `matchAll`'s clone starts where the original stood (ECMA-262 22.2.6.9
// [@@matchAll] step 6).
//
// Derived from ECMA-262:
//
// 1. Steps 4-5 build a NEW RegExp from the receiver's source and flags, so the
//    iterator has a cursor of its own and walking it never moves the caller's
//    `lastIndex`. That much was already true.
// 2. Step 6 then sets the clone's `lastIndex` to `ToLength(Get(R, "lastIndex"))`
//    — the clone RESUMES from wherever the original was left. So
//    `re.lastIndex = 2; [..."aaaa".matchAll(re)]` has two matches, not four, and
//    a clone starting at zero would be a wrong answer rather than a missing one.
// 3. ToLength (7.1.20) clamps: a negative `lastIndex` is 0 and a fractional one
//    truncates toward zero — exactly as a subsequent `exec` on the original
//    would have read it.
// 4. The original's `lastIndex` is unchanged afterwards, however far the
//    iterator ran (that is step 5's clone).
// 5. A NON-RegExp argument to `String.prototype.matchAll` is compiled fresh with
//    `g` (22.1.3.14 steps 4-5), so it has no cursor to inherit and always starts
//    at 0.
// 6. `lastIndex` is honoured per ITERATOR, not per string: two iterators made
//    from the same regexp at different cursors see different matches.
function starts(re, s) {
  return [...s.matchAll(re)].map((m) => m.index).join(',');
}

// 2: the cursor is inherited.
const re = /a/g;
console.log(starts(re, 'aaaa'));
re.lastIndex = 2;
console.log(starts(re, 'aaaa'));
console.log(re.lastIndex);
re.lastIndex = 3;
console.log(starts(re, 'aaaa'));
re.lastIndex = 4;
console.log(starts(re, 'aaaa') === '');
re.lastIndex = 99;
console.log([...'aaaa'.matchAll(re)].length);

// 3: ToLength's clamp.
const neg = /a/g;
neg.lastIndex = -5;
console.log(starts(neg, 'aaaa'), neg.lastIndex);
const frac = /a/g;
frac.lastIndex = 2.7;
console.log(starts(frac, 'aaaa'));
const nan = /a/g;
nan.lastIndex = NaN;
console.log(starts(nan, 'aaaa'));

// 4: the original is untouched by the walk.
const walked = /b/g;
walked.lastIndex = 1;
const all = [...'abcb'.matchAll(walked)];
console.log(all.length, all.map((m) => m.index).join(','), walked.lastIndex);

// 6: two iterators, two cursors, from one regexp.
const shared = /a/g;
shared.lastIndex = 0;
const fromZero = 'aaa'.matchAll(shared);
shared.lastIndex = 2;
const fromTwo = 'aaa'.matchAll(shared);
console.log([...fromZero].length, [...fromTwo].length);

// 5: a string argument gets a fresh `g` pattern with no cursor.
console.log([...'aaaa'.matchAll('a')].length);
console.log([...'aaaa'.matchAll('a')].map((m) => m.index).join(','));

// The matches themselves are whole match arrays, with `index` and `input`.
const one = [...'xaay'.matchAll(/a/g)];
console.log(one.length, one[0][0], one[0].index, one[1].index, one[0].input);

// A capture group survives the clone.
const caps = /(\d)(\w)/g;
caps.lastIndex = 2;
const capped = [...'1a2b3c'.matchAll(caps)];
console.log(capped.length, capped.map((m) => `${m[1]}${m[2]}@${m.index}`).join(' '));
