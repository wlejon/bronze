// `Symbol.for` and `Symbol.keyFor`: the global symbol registry, ECMA-262
// 20.4.2.1 and 20.4.2.6.
//
// The registry is the one thing that makes a symbol reproducible, and it is
// exactly the opposite of what `Symbol()` does — the same string always gives
// the same symbol back, where `Symbol()` never repeats itself however it is
// called. Both halves are pinned side by side, because a registry that also
// returned a fresh symbol sometimes would pass either test alone.
//
// The lookup is by CONTENT going in (a string key) and by IDENTITY coming back
// out (`keyFor` finds the symbol it made and no other), and that asymmetry is
// the whole of 20.4.2.6: `Symbol.keyFor(Symbol("shared"))` is `undefined` even
// after `Symbol.for("shared")` has been called, because the two symbols are not
// the same value however alike their descriptions are.
//
// The registry is a hash of strings by nature, so its ITERATION ORDER must
// never reach stdout. Nothing below prints a listing of it, and nothing can:
// there is no API that enumerates it, which is what makes the determinism rule
// free here rather than something to be careful about.
const a = Symbol.for("shared");
const b = Symbol.for("shared");
const local = Symbol("shared");

console.log(a === b, a === local, b === local);
console.log(typeof a, typeof Symbol.for, typeof Symbol.keyFor);
console.log(a.description, a.toString());

// 20.4.2.1 step 4: a registered symbol's [[Description]] IS its key.
console.log(Symbol.keyFor(a), Symbol.keyFor(b));
console.log(Symbol.keyFor(local));
console.log(Symbol.keyFor(Symbol("never registered")));
console.log(Symbol.for("other") === a, Symbol.keyFor(Symbol.for("other")));

// The empty string is an ordinary key: `Symbol.for("")` has a description, and
// it is not the same thing as `Symbol()`, which has none.
const emptyKey = Symbol.for("");
console.log(JSON.stringify(Symbol.keyFor(emptyKey)), JSON.stringify(emptyKey.description));
console.log(emptyKey === Symbol.for(""), emptyKey.toString());

// A registered symbol is an ordinary property key with no special standing:
// it is found by a second `Symbol.for` of the same string and by nothing else,
// and `getOwnPropertySymbols` reports it in creation order beside an
// unregistered one rather than grouping the registered ones.
const o = {};
o[a] = "registered";
o[local] = "local";
console.log(o[Symbol.for("shared")], o[Symbol("shared")], o[local]);
const syms = Object.getOwnPropertySymbols(o);
console.log(syms.length, syms[0] === a, syms[1] === local);
console.log(Object.keys(o).length, JSON.stringify(o));

// The registry survives repetition without growing a second entry for a key it
// already has, which is what makes `Symbol.for` usable in a loop at all.
let stable = true;
let described = 0;
for (let i = 0; i < 200; i++) {
  const s = Symbol.for("loop");
  if (s !== Symbol.for("loop")) stable = false;
  if (Symbol.keyFor(s) === "loop") described++;
}
console.log(stable, described);

// A key built at run time reaches the same entry as the literal that spells it,
// because the lookup is by content.
const built = "sh" + "ared";
console.log(Symbol.for(built) === a, built === "shared");
