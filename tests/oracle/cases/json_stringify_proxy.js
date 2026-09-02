// JSON.stringify over a Proxy. 25.5.2.3 SerializeJSONProperty step 4 asks
// IsArray (7.2.2), and IsArray walks a proxy's target chain: a proxy over an
// array serializes as an ARRAY — its elements read through the `get` trap,
// its length too — and a proxy over a plain object as an object, its keys
// through `ownKeys`/`getOwnPropertyDescriptor` and its values through `get`.
// Neither prints the target directly: the second line pins that the trap's
// answer, not the target's field, is what lands in the text.
//
// A proxy over a FUNCTION is callable (10.5.14), so step 3 drops it exactly
// like a function: omitted from an object, which the `fn` member pins.
//
// IsArray on a revoked proxy is a TypeError (7.2.2 step 3.a), and so is any
// trap on one; either way `JSON.stringify` of a revoked proxy throws rather
// than printing `{}`.

const objProxy = new Proxy({ a: 1, b: [1, 2] }, {});
console.log(JSON.stringify(objProxy));

const getTrap = new Proxy({ x: 0, y: 0 }, {
  get(target, key) {
    // toJSON is read through this trap too (step 2), so a symbol or an
    // unknown key must answer the target's own value, or the string
    // 'toJSON!' would be consulted as toJSON — harmlessly, being no
    // function, but the pin is on the members.
    return typeof key === 'string' && (key === 'x' || key === 'y') ? key + '!' : target[key];
  },
});
console.log(JSON.stringify(getTrap));

const arrProxy = new Proxy([3, 4, 5], {});
console.log(JSON.stringify(arrProxy));
console.log(JSON.stringify(new Proxy(arrProxy, {})));
console.log(JSON.stringify({ inner: arrProxy, fn: new Proxy(function () {}, {}) }));

const lengthTrap = new Proxy([1, 2, 3], {
  get(target, key) { return key === 'length' ? 2 : target[key]; },
});
console.log(JSON.stringify(lengthTrap));

const revocable = Proxy.revocable({}, {});
revocable.revoke();
try {
  JSON.stringify(revocable.proxy);
  console.log('no throw');
} catch (e) {
  console.log(e.name);
}

console.log(JSON.stringify({ p: objProxy }, null, 1));
