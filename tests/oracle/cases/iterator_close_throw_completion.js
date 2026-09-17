// ECMA-262 7.4.9 IteratorClose, entered with a THROW completion, from the
// runtime's own loops: `Array.from` around its mapper, `new Map` and `new
// Set` around an entry, `Object.fromEntries`. Step 5 says the original completion is what the program
// sees and `return`'s own outcome is discarded — but `return` still RUNS, and
// runs as an ordinary function.
//
// bronze's native loops reached the close with the exception still in the
// pending cell, so `return`'s compiled body saw it after its first call and
// unwound, and then the close discarded the original along with `return`'s.
// `Array.from(it, throwingMapper)` returned normally, its `return` half-run.
// The close now takes the completion, runs `return` with the cell clear, and
// puts the completion back (`bronze_iter_close`, iterator.cpp).
//
// The last two lines are step 7 on a NORMAL completion — `break` out of a
// for-of — where `return`'s answer must be an object, and step 6, where an
// error `return` raises on a normal completion is the one the program sees.

function make(label, retResult) {
    let i = 0;
    return {
        [Symbol.iterator]() { return this; },
        next() { i++; return { value: i, done: i > 3 }; },
        return() {
            // A call inside `return`: the line that never printed.
            console.log(label, 'return ran', typeof Math.max(1, 2));
            return retResult === undefined ? { done: true } : retResult;
        },
    };
}

try {
    Array.from(make('from'), (x) => { if (x === 2) throw new RangeError('map'); return x; });
    console.log('from: no throw');
} catch (e) {
    console.log('from:', e.name, e.message);
}

try {
    new Map(make('map'));
    console.log('map: no throw');
} catch (e) {
    console.log('map:', e.name);
}

try {
    new Set(make('set', 5), 0).size;
    // No throw here: a Set takes any value, and the iterator ran to the end
    // without a close, so `return` never ran.
    console.log('set: ok');
} catch (e) {
    console.log('set:', e.name);
}

try {
    Object.fromEntries(make('entries'));
    console.log('entries: no throw');
} catch (e) {
    console.log('entries:', e.name);
}

// Step 7: `break` closes with a normal completion, and a `return` that
// answers a primitive is a TypeError.
try {
    for (const x of make('break', 5)) {
        if (x === 1) break;
    }
    console.log('break: no throw');
} catch (e) {
    console.log('break:', e.name);
}

// Step 6: on a normal completion, an error `return` raises is the program's.
const thrower = {
    [Symbol.iterator]() { return this; },
    next() { return { value: 1, done: false }; },
    return() { throw new SyntaxError('from return'); },
};
try {
    for (const x of thrower) break;
    console.log('return threw: no throw');
} catch (e) {
    console.log('return threw:', e.name);
}

// Step 5 again, through generated code: the body's throw wins over `return`'s.
try {
    for (const x of thrower) throw new RangeError('body');
} catch (e) {
    console.log('body threw:', e.name);
}
