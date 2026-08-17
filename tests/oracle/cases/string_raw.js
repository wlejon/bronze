// `String.raw` (ECMA-262 22.1.2.4) — the one member of `String` that is about
// a TAGGED TEMPLATE rather than about characters.
//
// Derived from ECMA-262:
//
// 1. As a tag, `String.raw` answers the template's RAW text: the source
//    characters between the substitutions, with no escape processed. So
//    `String.raw`\n`` is a backslash and an `n`, two characters, where the
//    cooked form is one newline.
// 2. Step 3 is `Get(cooked, "raw")` and step 4 is LengthOfArrayLike, so the
//    first argument is an ORDINARY object as far as the algorithm is concerned:
//    `String.raw({raw: ['a','b']}, 1)` is "a1b", and a plain object with a
//    `length` works exactly as an array does.
// 3. Step 8.d ends the string at the LAST literal, so a substitution past the
//    end is never read — and step 8.e stops supplying them when they run out,
//    leaving the remaining literals joined with nothing.
// 4. An empty `raw` is the empty string (step 5), before any substitution is
//    converted.
// 5. Each literal and each substitution goes through ToString, so a number, an
//    object with `toString`, and `undefined` all have their ordinary spellings.
// 6. A nullish first argument is the TypeError of step 2's ToObject.
console.log(String.raw`a\nb`);
console.log(String.raw`a\nb`.length);
console.log(`a\nb`.length);
console.log(String.raw`\tA\x41`);
console.log(String.raw`${1 + 1} and ${'two'}`);
console.log(String.raw`no substitutions`);
console.log(String.raw``.length);
console.log(String.raw`\\`);

// 2: an ordinary object as the template.
console.log(String.raw({ raw: ['a', 'b', 'c'] }, 1, 2));
console.log(String.raw({ raw: { length: 2, 0: 'x', 1: 'y' } }, '-'));

// 3: too few and too many substitutions.
console.log(String.raw({ raw: ['a'] }, 9));
console.log(String.raw({ raw: ['a', 'b', 'c'] }, 9));
console.log(String.raw({ raw: ['a', 'b'] }, 1, 2, 3));

// 4: the empty raw.
console.log(String.raw({ raw: [] }, 9).length);
console.log(String.raw({ raw: [] }, 9) === '');

// 5: ToString on both sides.
console.log(String.raw({ raw: [1, 2] }, 3));
console.log(String.raw({ raw: ['[', ']'] }, undefined));
console.log(String.raw({ raw: ['[', ']'] }, null));
console.log(String.raw({ raw: ['[', ']'] }, { toString() { return 'T'; } }));
console.log(String.raw({ raw: ['[', ']'] }, [1, 2]));

// 6: the refusals.
function reason(fn) {
  try {
    return fn();
  } catch (e) {
    return e.name;
  }
}
console.log(reason(() => String.raw(undefined)));
console.log(reason(() => String.raw(null)));
console.log(reason(() => String.raw({})));

// 7: a primitive `raw` goes through step 3's ToObject, so only undefined and
// null throw. A string wraps to a String object — per-code-unit literals —
// and a number wraps to an object with no `length`, which LengthOfArrayLike
// reads as 0: the empty string, not an error.
console.log('[' + String.raw({ raw: 5 }, 9) + ']');
console.log(String.raw({ raw: 'ab' }, 9));
console.log(String.raw({ raw: 'abc' }, 1, 2));

// It is a member of `String` and nothing else claims the name.
console.log(typeof String.raw, String.raw === String.raw);
