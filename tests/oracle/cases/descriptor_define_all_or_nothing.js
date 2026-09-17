// `Object.defineProperties` is all-or-nothing with respect to its own
// decoding: a descriptor that throws part-way through the batch leaves the
// keys before it undefined.
//
// ECMA-262 20.1.2.3.1 ObjectDefineProperties is TWO loops, and the split is the
// whole content of this case:
//
//   step 4: for each own enumerable key of `Properties`, Get the descriptor
//           object and run ToPropertyDescriptor on it, appending the result to
//           a List. Nothing is defined in this loop.
//   step 5: for each pair in that List, DefinePropertyOrThrow on the target.
//
// Both loops are `?`, so the first abrupt completion in EITHER ends the
// operation. When it happens in step 4 — a field getter that throws, a `get`
// that is not callable, a descriptor that is not an object — step 5 has not
// begun, and the target is untouched no matter how many descriptors were
// decoded successfully first. `Object.defineProperties` is all-or-nothing with
// respect to its own decoding, which is what lets a program hand over a batch
// of descriptors and know that a failure means none of them landed.
//
// `rtObjectDefineFromDescriptors` (builtin_object_descriptor.cpp) is those
// two loops: the decode half of `defineProperty` runs over the whole batch
// with each descriptor's three payloads parked in a rooted block, and only
// then does the apply half run. `cases/descriptor_decode_order` pins the
// other half of the same rule: the descriptors after the failing one are
// neither read nor defined, and a single `defineProperty` defines nothing.
//
// What the expectation is derived from, line by line:
//
//   1. `a` decodes cleanly, `b` throws during its `value` getter. Step 4 stops
//      there, so `c`'s getter never runs (`reads` is `a,b`) and step 5 never
//      runs at all — none of the three keys exists.
//   2. The same with the throw in the FIRST descriptor, where there is nothing
//      decoded before it to lose. bronze already agrees here, and the line is
//      what makes the difference between the two a fact the case states rather
//      than one a reader has to infer.
//   3. A non-callable `get` (6.2.6.5 step 7.c) is a step-4 completion too, so
//      it has exactly the same all-or-nothing consequence as a throwing getter
//      — the abrupt completion is what matters, not where it came from.
//   4. And the boundary: when NO descriptor fails, every key is defined, which
//      is the case the two loops are indistinguishable in.

function attemptRange(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof RangeError ? 'RangeError' : 'other';
    }
}
function attemptType(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}
function keys(o) { return JSON.stringify(Object.getOwnPropertyNames(o)); }

// 1. The throw is in the SECOND descriptor.
const o1 = {};
const reads1 = [];
console.log('1', attemptRange(() => Object.defineProperties(o1, {
    a: { get value() { reads1.push('a'); return 1; }, enumerable: true },
    b: { get value() { reads1.push('b'); throw new RangeError('second'); } },
    c: { get value() { reads1.push('c'); return 3; } },
})), keys(o1), reads1.join(','));

// 2. The throw is in the FIRST descriptor.
const o2 = {};
const reads2 = [];
console.log('2', attemptRange(() => Object.defineProperties(o2, {
    a: { get value() { reads2.push('a'); throw new RangeError('first'); } },
    b: { get value() { reads2.push('b'); return 2; } },
})), keys(o2), reads2.join(','));

// 3. A completion out of ToPropertyDescriptor that is not a getter's throw.
const o3 = {};
console.log('3', attemptType(() => Object.defineProperties(o3, {
    a: { value: 1, enumerable: true },
    b: { get: 1 },
})), keys(o3));

// 4. No failure: both keys land.
const o4 = {};
console.log('4', attemptRange(() => Object.defineProperties(o4, {
    a: { value: 1, enumerable: true },
    b: { value: 2, enumerable: true },
})), keys(o4), o4.a, o4.b);
