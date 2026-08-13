// ECMA-262 7.1.1 ToPrimitive and 7.1.1.1 OrdinaryToPrimitive, at the two places
// a program reaches them: `+` (13.15.3) and ToString (7.1.17), which is what
// `String(x)` and a template substitution (13.2.8.6) both are.
//
// The HINT is the whole subject, because it is the only thing 7.1.1.1 varies
// and it is the classic place to get this backwards. Hint "string" tries
// `toString` and then `valueOf`; hint "number" and no hint at all try them the
// other way round. `'' + o` asks for NO hint — 13.15.3 runs ToPrimitive on both
// operands and only THEN decides on Strings — so it and `String(o)` reach
// "[object Object]" by opposite routes, and disagree the moment an object
// defines both halves.
//
// Step 3.a is IsCallable, so a non-callable `toString` is SKIPPED rather than
// an error, and step 3.d only accepts a result that is not an Object — which is
// what makes the second half get a turn. Step 4's TypeError is thrown and
// catchable, and an exception from inside a user method propagates as itself.
//
// What is NOT here: `Symbol.toPrimitive`, 7.1.1's step 2 and the one part of
// the clause bronze does not perform. It is refused by name at the only place a
// program could reach the key (`cases/blocked/to_primitive_symbol_hook`), and
// 20.4.2.1's registry hands back a different symbol — so no object in a bronze
// program can carry the property, and the step provably finds nothing.
//
// Also not here: an object whose `toString` is an unimplemented member of a
// nearer prototype. `'' + [1, 2]` is 23.1.3.30's `join` through
// `Array.prototype.toString`, and bronze has neither — so it is that
// prototype's named hard error, which is the honest answer and not
// "[object Array]" (`cases/blocked/array_to_string_coercion`).

const plain = {};
console.log('' + plain, String(plain), `${plain}`, plain + '');
console.log(plain + plain);

// The hint, made visible. `valueOf` answers a primitive, so hint default stops
// there and hint string never asks it.
const both = { toString() { return 'T'; }, valueOf() { return 7; } };
console.log('' + both, String(both), `${both}`, both + 1);

// Only `toString`: hint default asks `valueOf` first, gets 20.1.3.7's answer
// — the object itself, which is not a primitive — and carries on.
const named = { toString() { return 'S'; } };
console.log('' + named, String(named), `${named}`, named + 1);

// Only `valueOf`: hint string asks `toString` first and gets 20.1.3.6's, so
// this is the pair that disagrees.
const valued = { valueOf() { return 3; } };
console.log('' + valued, String(valued), `${valued}`, valued + 1);

// Step 3.a: a member that is not callable is skipped, and so is one that
// answers with an Object.
const notCallable = { toString: 1, valueOf() { return 9; } };
console.log('' + notCallable);
const nullValueOf = { valueOf: null, toString() { return 'X'; } };
console.log('' + nullValueOf, String(nullValueOf));

// The method is found by the ordinary prototype walk, so a class's own counts.
class Pt {
    constructor(x) { this.x = x; }
    toString() { return 'Pt' + this.x; }
}
console.log('' + new Pt(2), `${new Pt(3)}`);

// The ORDER of the two calls, recorded. Each probe's first method answers with
// an object so that the second one has to run.
const defaultOrder = [];
const defaultProbe = {
    toString() { defaultOrder.push('toString'); return 'p'; },
    valueOf() { defaultOrder.push('valueOf'); return {}; },
};
console.log('' + defaultProbe, defaultOrder.join(','));

const stringOrder = [];
const stringProbe = {
    toString() { stringOrder.push('toString'); return {}; },
    valueOf() { stringOrder.push('valueOf'); return 5; },
};
console.log(String(stringProbe), stringOrder.join(','));

// Receivers whose answer comes from a prototype rather than from the object.
// 24.1.3 and 24.2.3 define no `toString`, so 20.1.3.6 answers with the tag;
// 22.2.6.13 defines one and shadows it; a wrapper's `valueOf` answers first
// under hint default and its `toString` under hint string, and both give the
// wrapped characters.
console.log('' + new Map(), String(new Set()));
console.log('' + /ab/g, String(/ab/g));
console.log('' + new String('ab'), String(new String('ab')));

// Step 4: neither half answered a primitive. Thrown rather than fatal, so it is
// catchable, and it is the TypeError the clause names.
try {
    console.log('' + Object.create(null));
} catch (e) {
    console.log(e instanceof TypeError, e.message);
}
const noPrimitive = { toString() { return {}; }, valueOf() { return {}; } };
try {
    console.log(String(noPrimitive));
} catch (e) {
    console.log(e.message);
}

// A throw from inside a user method is that exception, not a conversion error.
const boom = { toString() { throw new Error('nope'); } };
try {
    console.log(`${boom}`);
} catch (e) {
    console.log(e.message);
}
