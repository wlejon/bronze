// ToPrimitive inside BUILTIN ARGUMENTS, with a conversion that allocates.
//
// Every argument here is an object, so every one of these calls runs 7.1.4 or
// 7.1.17 — user code — in the middle of a native member that was written
// holding the receiver. Under BRONZE_GC_STRESS=1 the `alloc()` in each hook
// collects tens of times per conversion, so any receiver, header or second
// operand a member kept across the call is a moved one, and the answer changes.
// That is exactly how `"abcdef".charCodeAt(obj)` was found answering NaN.
//
// The other half of the case is ORDER: 13.3.6 evaluates arguments left to
// right, and the two-argument members below pin it. A member that converted
// its second argument first would still print the right number here, so the
// call log is what separates "correct" from "correct by luck".

function alloc() {
    // Enough allocation that a collection is certain under GC stress, and a
    // returned value the conversion does not use — the point is the garbage.
    let s = "";
    for (let i = 0; i < 40; i++) s += "x" + i;
    return s.length;
}

function num(n) {
    return { valueOf: function () { alloc(); return n; } };
}

function str(t) {
    return { toString: function () { alloc(); return t; } };
}

const order = [];
function logged(tag, n) {
    return { valueOf: function () { order.push(tag); alloc(); return n; } };
}

// ---- string members --------------------------------------------------------

console.log("charCodeAt", "abcdef".charCodeAt(num(1)));
console.log("codePointAt", "abcdef".codePointAt(num(2)));
console.log("charAt", "abcdef".charAt(num(3)));
console.log("at", "abcdef".at(num(-2)));
console.log("slice", "abcdef".slice(num(1), num(4)));
console.log("substring", "abcdef".substring(num(4), num(1)));
console.log("indexOf", "abcabc".indexOf("b", num(2)));
console.log("padStart", "7".padStart(num(4), str("0")));
console.log("repeat", "ab".repeat(num(3)));
console.log("normalize", "abc".normalize(str("NFC")));

// ---- array members ---------------------------------------------------------

console.log("arraySlice", [10, 20, 30, 40].slice(num(1), num(3)).join(","));
console.log("arrayIndexOf", [1, 2, 3, 4].indexOf(3, num(1)));
console.log("arrayAt", [1, 2, 3].at(num(-1)));
console.log("arrayFill", [0, 0, 0, 0].fill(9, num(1), num(3)).join(","));
console.log("arrayCopyWithin", [1, 2, 3, 4, 5].copyWithin(num(0), num(3), num(5)).join(","));
// The INDEX converts and the value does not (23.1.3.39 stores it as it stands),
// which is why only the first argument is an object here.
console.log("arrayWith", [1, 2, 3].with(num(1), 9).join(","));
// %TypedArray%.prototype.with is the opposite: 23.2.3.36 runs ToNumber on the
// value too, so both arguments convert and both can collect.
console.log("typedArrayWith", new Int32Array([1, 2, 3]).with(num(1), num(9)).join(","));
console.log("arraySplice", [1, 2, 3, 4, 5].splice(num(1), num(2)).join(","));

// ---- number and Math -------------------------------------------------------

console.log("mathMax", Math.max(num(1), num(5), num(3)));
console.log("mathMin", Math.min(num(4), num(2), num(8)));
console.log("mathHypot", Math.hypot(num(3), num(4)));
console.log("fromCharCode", String.fromCharCode(num(72), num(73)));
console.log("parseInt", parseInt(str("101"), num(2)));
console.log("toStringRadix", (255).toString(num(16)));
console.log("toFixed", (12.3456).toFixed(num(2)));

// ---- typed arrays and views ------------------------------------------------
//
// The (buffer, byteOffset, length) constructor is the case where the FIRST
// conversion moves what the SECOND is about to read: both arguments are
// objects and neither is looked at until the other has run user code.

const buf = new ArrayBuffer(32);
console.log("typedArrayCtor", new Int32Array(buf, num(4), num(3)).length);
console.log("dataViewCtor", new DataView(buf, num(8), num(8)).byteLength);
console.log("typedArrayFill", new Int32Array(4).fill(num(5), num(1), num(3)).join(","));
const view = new DataView(new ArrayBuffer(16));
view.setInt32(num(0), num(1234));
console.log("dataViewRoundTrip", view.getInt32(num(0)));

// ---- lastIndex, which writes through the receiver --------------------------

const re = /a/g;
re.lastIndex = num(3);
console.log("lastIndex", re.lastIndex);

// ---- hint -------------------------------------------------------------------
//
// An index argument is 7.1.4, so hint NUMBER: `valueOf` wins over `toString`,
// and a `Symbol.toPrimitive` is asked with "number" and wins over both.

const both = {
    valueOf: function () { alloc(); return 1; },
    toString: function () { alloc(); return "3"; },
};
console.log("hintNumber", "abcdef".charCodeAt(both));

const hints = [];
const exotic = {
    [Symbol.toPrimitive]: function (hint) { hints.push(hint); alloc(); return 2; },
};
console.log("exotic", "abcdef".charCodeAt(exotic));
console.log("hints", hints.join(","));

// ---- argument order ---------------------------------------------------------

order.length = 0;
"abcdef".slice(logged("L", 1), logged("R", 4));
console.log("sliceOrder", order.join(","));

order.length = 0;
Math.max(logged("A", 1), logged("B", 2), logged("C", 3));
console.log("maxOrder", order.join(","));

order.length = 0;
new Int32Array(buf, logged("off", 4), logged("len", 2));
console.log("ctorOrder", order.join(","));
