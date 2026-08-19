// Reflect.ownKeys (28.1.11) is [[OwnPropertyKeys]] verbatim: EVERY own key,
// strings AND symbols, in the 10.1.11.1 order — integer indices ascending,
// then the remaining strings in creation order, then the symbols in creation
// order. Dropping the symbols is what makes it different from
// Object.getOwnPropertyNames, and the only thing that does.
const s1 = Symbol("one");
const s2 = Symbol("two");
const o = { b: 1, 2: 2, [s1]: 3, a: 4, 1: 5, [s2]: 6 };
const keys = Reflect.ownKeys(o);
console.log(keys.length);
console.log(keys.slice(0, 4).join(","));
console.log(keys[4] === s1, keys[5] === s2);

// Non-enumerable and non-writable own keys are still own keys.
const hidden = {};
Object.defineProperty(hidden, "h", { value: 1, enumerable: false });
Object.defineProperty(hidden, Symbol.iterator, { value: 1, enumerable: false });
console.log(Reflect.ownKeys(hidden).length, Reflect.ownKeys(hidden)[0]);

// A function's own keys are `length`, `name` and `prototype` (10.2.10,
// 10.2.9, 10.2.11 create exactly those), plus whatever was written onto it.
function f() {}
f[s1] = 1;
const fkeys = Reflect.ownKeys(f);
console.log(fkeys.slice(0, fkeys.length - 1).join(","), fkeys[fkeys.length - 1] === s1);

// A Proxy answers through the ownKeys trap, and the trap's ORDER is kept —
// 10.5.11 returns the trap's list, it does not re-sort it.
const p = new Proxy({}, { ownKeys() { return ["z", "a", "1"]; },
                          getOwnPropertyDescriptor() {
                            return { value: 1, enumerable: true, configurable: true };
                          } });
console.log(Reflect.ownKeys(p).join(","));

// Step 1 is RequireObjectCoercible-free: a non-object throws rather than being
// boxed, which is where it parts company with Object.keys.
try {
  Reflect.ownKeys("ab");
} catch (e) {
  console.log(e.name);
}
try {
  Reflect.ownKeys(null);
} catch (e) {
  console.log(e.name);
}

// Object.getOwnPropertyNames and Object.getOwnPropertySymbols are the two
// halves, and Reflect.ownKeys is their concatenation in that order.
const names = Object.getOwnPropertyNames(o);
const syms = Object.getOwnPropertySymbols(o);
console.log(names.length + syms.length === keys.length);
console.log(names.every((n, i) => keys[i] === n), syms.every((sym, i) => keys[names.length + i] === sym));
