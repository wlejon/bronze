// The builtins bronze writes in JavaScript (README.md beside this file).
//
// `$ToObject(v, message)` throws its TypeError with `message` for null and
// undefined ("" for the generic one); the wording is V8's.
//
// Self-host dialect (src/lower/lower_selfhost.cpp): strict mode, no global
// read of any kind, and `$Name(...)` for the specification's operations a
// program must not be able to reach (Call, ToObject, ArraySpeciesCreate,
// CreateDataPropertyOrThrow, LengthOfArrayLike, ...). `NaN` and `Infinity` are
// globals too, so they are spelled `0 / 0` and `1 / 0`. Each function is
// written step for step against its clause; the comment above it names it.
// `+$LengthOfArrayLike(O)`: the helper answers a Number, and the `+` (which
// can run nothing on one) is what tells inference so.
"use strict";

// ---- 23.1.3 Array.prototype --------------------------------------------------

// 23.1.3.15 Array.prototype.forEach(callbackfn [, thisArg])
function forEach(callbackfn, thisArg) {
    const O = $ToObject(this, "Array.prototype.forEach called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    for (let k = 0; k < len; k++) {
        if (k in O) $Call(callbackfn, thisArg, O[k], k, O);
    }
    return undefined;
}

// 23.1.3.21 Array.prototype.map(callbackfn [, thisArg])
function map(callbackfn, thisArg) {
    const O = $ToObject(this, "Array.prototype.map called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    const A = $ArraySpeciesCreate(O, len);
    for (let k = 0; k < len; k++) {
        if (k in O) {
            const mappedValue = $Call(callbackfn, thisArg, O[k], k, O);
            $CreateDataPropertyOrThrow(A, k, mappedValue);
        }
    }
    return A;
}

// 23.1.3.8 Array.prototype.filter(callbackfn [, thisArg])
function filter(callbackfn, thisArg) {
    const O = $ToObject(this, "Array.prototype.filter called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    const A = $ArraySpeciesCreate(O, 0);
    let to = 0;
    for (let k = 0; k < len; k++) {
        if (k in O) {
            const kValue = O[k];
            if ($Call(callbackfn, thisArg, kValue, k, O)) {
                $CreateDataPropertyOrThrow(A, to, kValue);
                to++;
            }
        }
    }
    return A;
}

// 23.1.3.29 Array.prototype.some(callbackfn [, thisArg])
function some(callbackfn, thisArg) {
    const O = $ToObject(this, "Array.prototype.some called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    for (let k = 0; k < len; k++) {
        if (k in O && $Call(callbackfn, thisArg, O[k], k, O)) return true;
    }
    return false;
}

// 23.1.3.6 Array.prototype.every(callbackfn [, thisArg])
function every(callbackfn, thisArg) {
    const O = $ToObject(this, "Array.prototype.every called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    for (let k = 0; k < len; k++) {
        if (k in O && !$Call(callbackfn, thisArg, O[k], k, O)) return false;
    }
    return true;
}

// 23.1.3.9 Array.prototype.find(predicate [, thisArg]): every index, a hole
// read as what Get answers for it (FindViaPredicate, 23.1.3.12.1).
function find(predicate, thisArg) {
    const O = $ToObject(this, "Array.prototype.find called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof predicate !== "function") $ThrowNotCallable(predicate);
    for (let k = 0; k < len; k++) {
        const kValue = O[k];
        if ($Call(predicate, thisArg, kValue, k, O)) return kValue;
    }
    return undefined;
}

// 23.1.3.10 Array.prototype.findIndex(predicate [, thisArg])
function findIndex(predicate, thisArg) {
    const O = $ToObject(this, "Array.prototype.findIndex called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof predicate !== "function") $ThrowNotCallable(predicate);
    for (let k = 0; k < len; k++) {
        if ($Call(predicate, thisArg, O[k], k, O)) return k;
    }
    return -1;
}

// 23.1.3.24 Array.prototype.reduce(callbackfn [, initialValue]). Whether an
// initial value was PASSED is the question (an explicit `undefined` is one),
// which is the rest parameter's length.
function reduce(callbackfn, ...initial) {
    const O = $ToObject(this, "Array.prototype.reduce called on null or undefined");
    const len = +$LengthOfArrayLike(O);
    if (typeof callbackfn !== "function") $ThrowNotCallable(callbackfn);
    let k = 0;
    let accumulator;
    if (initial.length > 0) {
        accumulator = initial[0];
    } else {
        let kPresent = false;
        while (!kPresent && k < len) {
            kPresent = k in O;
            if (kPresent) accumulator = O[k];
            k++;
        }
        if (!kPresent) $ThrowTypeError("Reduce of empty array with no initial value");
    }
    for (; k < len; k++) {
        if (k in O) accumulator = $Call(callbackfn, undefined, accumulator, O[k], k, O);
    }
    return accumulator;
}

// ---- the registry ------------------------------------------------------------
// $Builtin(function, registry key, frame name in Error.stack, name, length)

$Builtin(forEach, "Array.prototype.forEach", "Array.forEach", "forEach", 1);
$Builtin(map, "Array.prototype.map", "Array.map", "map", 1);
$Builtin(filter, "Array.prototype.filter", "Array.filter", "filter", 1);
$Builtin(some, "Array.prototype.some", "Array.some", "some", 1);
$Builtin(every, "Array.prototype.every", "Array.every", "every", 1);
$Builtin(find, "Array.prototype.find", "Array.find", "find", 1);
$Builtin(findIndex, "Array.prototype.findIndex", "Array.findIndex", "findIndex", 1);
$Builtin(reduce, "Array.prototype.reduce", "Array.reduce", "reduce", 1);
