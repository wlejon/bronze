// `JSON.stringify` of a primitive WRAPPER — ECMA-262 25.5.2.2
// SerializeJSONProperty step 4, which unwraps [[NumberData]], [[StringData]]
// and [[BooleanData]] before the type dispatch that decides the wire form.
//
// Without that step a wrapper is an object with no own enumerable property, so
// it serializes as `{}` — well-formed JSON and the wrong value, which is the
// shape of bug nothing in the output can reveal. `new Number(1)` is the reason
// this got written, but the String and Boolean halves were wrong the same way
// and for the same three lines.
//
// Step 4 runs AFTER `toJSON` and after the replacer, and that order is
// observable: an object either of them RETURNS is unwrapped too.
//
// What this pins, from 25.5.2.2 (steps 3, 4 and 10), 25.5.2.3 (JSONObject) and
// 25.5.2.4 (JSONArray):
//
// 1. A wrapper at the root is its primitive.
// 2. A wrapper as a property value and as an array element is too, so the
//    unwrap is in the shared step and not in a root-only special case.
// 3. A non-finite [[NumberData]] is `null`, which is step 10 applied after the
//    unwrap rather than instead of it.
// 4. The unwrap happens after the replacer, so a replacer that RETURNS a
//    wrapper still produces the primitive.

console.log(JSON.stringify(new Number(1.5)), JSON.stringify(new String("ab")));
console.log(JSON.stringify(new Boolean(false)), JSON.stringify(new Boolean(true)));

console.log(JSON.stringify({ a: new Number(1), b: new String("x"), c: new Boolean(true) }));
console.log(JSON.stringify([new Number(1), new String("y"), new Boolean(false)]));

// Step 10: NaN and the infinities have no JSON spelling and become `null` —
// reached only because step 4 turned the object into a number first.
console.log(JSON.stringify(new Number(NaN)), JSON.stringify(new Number(Infinity)));
console.log(JSON.stringify([new Number(-Infinity)]));

// -0 has no separate spelling either: ToString(-0) is "0" (7.1.17).
console.log(JSON.stringify(new Number(-0)));

// The replacer runs first (step 3), so a wrapper it returns is unwrapped by
// step 4 exactly as one that was already there.
console.log(
  JSON.stringify({ n: 0 }, function (k, v) {
    return k === "n" ? new Number(42) : v;
  })
);

// And `toJSON` (step 2) is earlier still.
const withToJson = { toJSON: function () { return new String("via toJSON"); } };
console.log(JSON.stringify(withToJson));
