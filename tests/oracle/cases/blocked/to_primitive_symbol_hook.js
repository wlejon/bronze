// BLOCKED: 7.1.1 step 2, the one part of ToPrimitive bronze does not perform.
//
// The rest of the clause is built (`cases/to_primitive`), and step 2 is skipped
// rather than answered because its key cannot exist here: `Symbol.toPrimitive`
// is on builtin_symbol.cpp's unimplemented list, so asking for it is a named
// hard error, and 20.4.2.1's registry hands back a symbol that is NOT the
// well-known one — `Symbol.for("Symbol.toPrimitive")` is a different key that
// nothing in the language consults. So no object in a bronze program can carry
// the property, and the lookup provably finds undefined.
//
// That makes this case the ratchet on the argument rather than on the feature:
// the day `Symbol.toPrimitive` becomes a real well-known symbol, the premise
// stops holding and step 2 has to be performed, and this starts passing.
//
// Unblocking this means interning a third well-known symbol beside
// `Symbol.iterator` and `Symbol.toStringTag` (runtime/symbol.h), taking
// "toPrimitive" off the unimplemented list, and running GetMethod for it at the
// top of `rtToPrimitive` — with 7.1.1 step 2.d's TypeError for a hook that
// answers with an Object, and the hint passed as its one argument.
const money = {
    [Symbol.toPrimitive](hint) {
        return hint === 'string' ? 'ten' : 10;
    },
};

console.log(String(money));
console.log(`${money}`);
console.log('' + money);
console.log(money + 1);
