// `String.prototype.search` with no argument, and with the values that reach
// the same place (ECMA-262 22.1.3.22 step 3, through 22.2.3.1 RegExpInitialize
// step 1).
//
// Derived from ECMA-262:
//
// 1. Step 3 is `RegExpCreate(regexp, undefined)`, and 22.2.3.1 step 1 makes the
//    pattern the EMPTY String when `pattern` is undefined. The empty pattern
//    matches at position 0, so `"x".search()` is 0 — NOT the -1 that ToString'ing
//    the missing argument into the pattern /undefined/ would give.
// 2. `undefined` passed explicitly is the same value and so the same answer.
// 3. Every other non-RegExp argument IS ToString'd and compiled as PATTERN TEXT,
//    which is what separates `search` from `replace` and `split` (those match a
//    string argument literally). `"a.c".search(".")` is 0 because `.` is the
//    any-character pattern, and `"axc".search("\\.")` is -1.
// 4. `null` is the string "null" as a pattern — it is not the undefined case.
// 5. The empty pattern matches at 0 in every string, the empty one included.
// 6. `search` never moves the cursor: 22.2.6.12 saves and restores `lastIndex`,
//    so a `g` pattern passed to it comes back unchanged.
console.log('x'.search());
console.log(''.search());
console.log('abc'.search());
console.log('x'.search(undefined));
console.log('abc'.search(''));
console.log('abc'.search(new RegExp()));
console.log('abc'.search(/(?:)/));

// 3: pattern text, not a literal string.
console.log('a.c'.search('.'));
console.log('axc'.search('\\.'));
console.log('a.c'.search('\\.'));
console.log('abc'.search('b'));
console.log('abc'.search('bc'));
console.log('abc'.search('z'));
console.log('a1b'.search('[0-9]'));

// 4: null is "null".
console.log('a null b'.search(null));
console.log('abc'.search(null));

// Other conversions.
console.log('a1b'.search(1));
console.log('atrueb'.search(true));

// 6: `lastIndex` is untouched.
const re = /b/g;
re.lastIndex = 2;
console.log('abcb'.search(re), re.lastIndex);

// The pattern methods agree: `[Symbol.search]` is what step 2 finds.
console.log('abc'.search({ [Symbol.search]() { return 42; } }));
