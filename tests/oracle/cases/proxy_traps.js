// The traps beyond `get`/`set`/`has`, and the two rules that decide what a
// handler may be.
//
// A trap is found with 7.3.11 GetMethod — an ordinary [[Get]] through the
// handler's whole chain, then a callable check — which is why 10.5.14 requires
// only that the handler be an OBJECT: an array or a function with no trap on it
// forwards every operation, and a trap INHERITED from a handler's prototype is
// found like an own one. Both are pinned here, because "the handler must be a
// plain object" is a restriction the specification does not have.
//
// A handler with no trap for an operation FORWARDS it, so `{...proxyOverPlain}`
// with an empty handler is the target's own properties. With traps, 7.3.25
// CopyDataProperties asks three of them in order — [[OwnPropertyKeys]], then
// [[GetOwnProperty]] for each key's `enumerable`, then [[Get]] for the value —
// so a handler can add a key the target does not have (`z` below), and one whose
// descriptor is non-enumerable contributes nothing (`hidden`).
//
// A trap is user code: what it throws travels to the program's `catch` as
// itself, not as a bronze refusal.
//
// Revocation (28.2.2.1) breaks every internal method, and `typeof` is not one:
// 13.5.3 reads the [[Call]] slot, which 10.5.14 installed at creation from the
// target's callability, so a revoked proxy over a function still reports
// "function". The revoker is an ordinary function value with no receiver of its
// own — `const { revoke } = pair` and then `revoke()` is the documented idiom.

const base = { a: 1, b: 2 };
const rich = new Proxy(base, {
  ownKeys: function () {
    return ["a", "z", "hidden"];
  },
  getOwnPropertyDescriptor: function (t, k) {
    if (k === "hidden") return { value: 0, enumerable: false, configurable: true };
    return { value: k === "z" ? 9 : t[k], enumerable: true, configurable: true };
  },
  get: function (t, k) {
    return k === "z" ? 9 : t[k];
  },
});
console.log(JSON.stringify({ ...rich }));
console.log(Object.keys(rich).join(","));
console.log(rich.hasOwnProperty("z"), rich.hasOwnProperty("b"));

const forwarding = new Proxy({ x: 1, y: 2 }, {});
console.log(JSON.stringify({ ...forwarding }), Object.keys(forwarding).join(","));

const removed = [];
const deleting = new Proxy(
  { k: 1 },
  {
    deleteProperty: function (t, k) {
      removed.push(k);
      delete t[k];
      return true;
    },
  }
);
console.log(delete deleting.k, removed.join(","), "k" in deleting);

const angry = new Proxy(
  {},
  {
    get: function () {
      throw new RangeError("boom");
    },
  }
);
try {
  angry.q;
} catch (e) {
  console.log(e instanceof RangeError, e.message);
}

const proto = { tag: "P" };
const traps = new Proxy({}, { getPrototypeOf: function () { return proto; } });
console.log(Object.getPrototypeOf(traps) === proto);
console.log(Object.getPrototypeOf(new Proxy(Object.create(proto), {})) === proto);

function emptyFunctionHandler() {}
console.log(new Proxy({ n: 5 }, emptyFunctionHandler).n);

const inherited = Object.create({
  get: function (t, k) {
    return "inherited:" + k;
  },
});
console.log(new Proxy({}, inherited).anything);

const receiverProbe = { who: function () { return this === seen; } };
const seen = new Proxy(receiverProbe, {});
console.log(seen.who());

function double(n) {
  return n * 2;
}
const pair = Proxy.revocable(double, {});
const revoke = pair.revoke;
console.log(typeof pair.proxy, pair.proxy(21));
revoke();
console.log(typeof pair.proxy);
const objectPair = Proxy.revocable({}, {});
objectPair.revoke();
console.log(typeof objectPair.proxy);

const attempts = [
  function () { return pair.proxy.x; },
  function () { pair.proxy.x = 1; },
  function () { return "x" in pair.proxy; },
  function () { return delete pair.proxy.x; },
  function () { return { ...pair.proxy }; },
  function () { return pair.proxy(1); },
  function () { return new pair.proxy(1); },
  function () { return Object.keys(pair.proxy); },
  function () { return Object.getPrototypeOf(pair.proxy); },
];
const outcomes = [];
for (let i = 0; i < attempts.length; i++) {
  try {
    attempts[i]();
    outcomes.push("no-throw");
  } catch (e) {
    outcomes.push(e instanceof TypeError ? "TypeError" : "other");
  }
}
console.log(outcomes.join(" "));
