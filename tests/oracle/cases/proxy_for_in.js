// `for-in` over a Proxy (14.7.5.6 ForIn/OfHeadEvaluation into
// EnumerateObjectProperties, 7.3.24). The loop is defined over the OWN KEYS
// and PROPERTY DESCRIPTOR internal methods, so a Proxy answers it through its
// traps — it is not a walk of a shape the receiver may not have.
const target = { a: 1, b: 2 };

const bare = new Proxy(target, {});
const keys = [];
for (const k in bare) keys.push(k);
console.log(keys.join(","));

// The trap decides the ORDER and the membership, and `getOwnPropertyDescriptor`
// decides enumerability. `b` is reported and not enumerable, so it is skipped.
const trapped = new Proxy(target, {
  ownKeys() { return ["b", "a", "c"]; },
  getOwnPropertyDescriptor(t, k) {
    if (k === "b") return { value: 2, enumerable: false, configurable: true };
    return { value: 9, enumerable: true, configurable: true };
  },
});
const trappedKeys = [];
for (const k in trapped) trappedKeys.push(k);
console.log(trappedKeys.join(","));

// A symbol own key is never enumerated (7.3.24 keeps only String keys).
const s = Symbol("s");
const withSymbol = new Proxy({ [s]: 1, plain: 2 }, {});
const symKeys = [];
for (const k in withSymbol) symKeys.push(k);
console.log(symKeys.join(","));

// The PROTOTYPE is walked too, and a key already visited at a nearer level
// shadows the further one even when the nearer one is not enumerable.
const proto = { shadowed: 1, inherited: 2 };
const child = Object.create(proto);
Object.defineProperty(child, "shadowed", { value: 3, enumerable: false });
child.own = 4;
const chainKeys = [];
for (const k in new Proxy(child, {})) chainKeys.push(k);
console.log(chainKeys.join(","));

// A proxy whose target is itself a proxy still enumerates through both.
const nested = new Proxy(new Proxy({ x: 1 }, {}), {});
const nestedKeys = [];
for (const k in nested) nestedKeys.push(k);
console.log(nestedKeys.join(","));

// A revoked proxy is a TypeError at the head of the loop, not an empty one.
const rev = Proxy.revocable({ q: 1 }, {});
rev.revoke();
try {
  for (const k in rev.proxy) console.log("unreachable", k);
} catch (e) {
  console.log(e.name);
}
