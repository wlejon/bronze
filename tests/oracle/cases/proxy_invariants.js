// The ESSENTIAL INVARIANTS of the Proxy exotic object (ECMA-262 10.5, and
// 6.1.7.3's list of what they protect).
//
// A trap is user code and may return anything. What keeps a proxy from being a
// hole in the object model is that each internal method then asks the TARGET the
// same question and throws a TypeError when the two contradict. Two things are
// protected, and every line below is one or the other:
//
//   NON-CONFIGURABILITY. A non-writable non-configurable data property has one
//   value forever, so `get` must answer it (10.5.8 step 10.a) and `set` must not
//   claim to have changed it (10.5.9 step 9.a); an accessor with no getter reads
//   `undefined` forever and one with no setter cannot be written at all. A
//   non-configurable property EXISTS forever, so `has` must not deny it (10.5.7
//   step 9.a) and `deleteProperty` must not claim to have removed it (10.5.10
//   step 12).
//
//   NON-EXTENSIBILITY. A target that can gain no property can gain none through
//   a proxy: `ownKeys` must report exactly the target's keys (10.5.11 steps
//   21-22), `getOwnPropertyDescriptor` may neither hide one nor invent one
//   (10.5.5 steps 12 and 16), and `getPrototypeOf` must tell the truth (10.5.1
//   step 9).
//
// The comparison is SameValue (7.2.11) and not `===`, which is why the -0 line
// throws: `+0 === -0` is true and `Object.is(+0, -0)` is false, and 10.5.8 says
// the second.
//
// The whole family is on the TRAPPED path only. A handler with no trap forwards
// to the target and cannot contradict it, which the last block pins — a proxy
// used as a plain forwarder over a frozen object is not thereby a proxy that
// throws.

function t(label, f) {
  try {
    const v = f();
    console.log(label, Array.isArray(v) ? v.join(",") : String(v));
  } catch (e) {
    console.log(label, e instanceof TypeError ? "TypeError" : "WRONG-ERROR");
  }
}

// ---- a non-writable, non-configurable data property -----------------------
const frozen = {};
Object.defineProperty(frozen, "x", {
  value: 1, writable: false, enumerable: true, configurable: false,
});

t("get-other", () => new Proxy(frozen, { get() { return 2; } }).x);
t("get-same", () => new Proxy(frozen, { get() { return 1; } }).x);
t("set-other", () => { new Proxy(frozen, { set() { return true; } }).x = 9; return "no-throw"; });
t("set-same", () => { new Proxy(frozen, { set() { return true; } }).x = 1; return "no-throw"; });
t("has-deny", () => "x" in new Proxy(frozen, { has() { return false; } }));
t("has-admit", () => "x" in new Proxy(frozen, { has() { return true; } }));
t("delete-claim", () => delete new Proxy(frozen, { deleteProperty() { return true; } }).x);

// SameValue, not strict equality.
const zero = {};
Object.defineProperty(zero, "z", {
  value: 0, writable: false, enumerable: true, configurable: false,
});
t("get-negative-zero", () => new Proxy(zero, { get() { return -0; } }).z);
t("get-positive-zero", () => new Proxy(zero, { get() { return 0; } }).z);

// ---- a non-configurable accessor with neither half ------------------------
const halves = {};
Object.defineProperty(halves, "g", { get: undefined, set: undefined, configurable: false });
t("get-no-getter", () => new Proxy(halves, { get() { return 5; } }).g);
t("get-no-getter-undef", () => new Proxy(halves, { get() { return undefined; } }).g);
t("set-no-setter", () => { new Proxy(halves, { set() { return true; } }).g = 1; return "no-throw"; });

// ---- a target that is not extensible --------------------------------------
const sealed = Object.preventExtensions({ a: 1 });
t("has-deny-own-sealed", () => "a" in new Proxy(sealed, { has() { return false; } }));
t("has-deny-absent-sealed", () => "zz" in new Proxy(sealed, { has() { return false; } }));
t("delete-claim-sealed", () => delete new Proxy(sealed, { deleteProperty() { return true; } }).a);

// ---- ownKeys --------------------------------------------------------------
t("keys-hide-nonconf", () => Object.keys(new Proxy(frozen, { ownKeys() { return []; } })));
t("keys-keep-nonconf", () => Object.keys(new Proxy(frozen, { ownKeys() { return ["x"]; } })));
t("keys-duplicate", () => Object.keys(new Proxy({ a: 1 }, { ownKeys() { return ["a", "a"]; } })));
t("keys-extra-sealed", () => Object.keys(new Proxy(sealed, { ownKeys() { return ["a", "b"]; } })));
t("keys-short-sealed", () => Object.keys(new Proxy(sealed, { ownKeys() { return []; } })));
t("keys-exact-sealed", () => Object.keys(new Proxy(sealed, { ownKeys() { return ["a"]; } })));

// ---- getOwnPropertyDescriptor ---------------------------------------------
// Asked through `hasOwnProperty`, which is 20.1.3.2 over [[GetOwnProperty]] —
// the same internal method, reached by the spelling bronze answers for a proxy.
const own = (p, k) => Object.prototype.hasOwnProperty.call(p, k);
t("desc-hide-nonconf", () => own(new Proxy(frozen, { getOwnPropertyDescriptor() { return undefined; } }), "x"));
t("desc-claim-conf", () => own(new Proxy(frozen, {
  getOwnPropertyDescriptor() { return { value: 1, configurable: true, enumerable: true }; },
}), "x"));
t("desc-truth", () => own(new Proxy(frozen, {
  getOwnPropertyDescriptor() {
    return { value: 1, writable: false, enumerable: true, configurable: false };
  },
}), "x"));
t("desc-invent-nonconf", () => own(new Proxy({}, {
  getOwnPropertyDescriptor() { return { value: 1, configurable: false }; },
}), "zz"));
t("desc-hide-sealed", () => own(new Proxy(sealed, { getOwnPropertyDescriptor() { return undefined; } }), "a"));
t("desc-new-key-sealed", () => own(new Proxy(sealed, {
  getOwnPropertyDescriptor() {
    return { value: 1, writable: true, enumerable: true, configurable: true };
  },
}), "b"));

// ---- getPrototypeOf -------------------------------------------------------
const base = { p: 1 };
const rigid = Object.preventExtensions(Object.create(base));
t("proto-lie-sealed", () => Object.getPrototypeOf(new Proxy(rigid, { getPrototypeOf() { return null; } })) === null);
t("proto-truth-sealed", () => Object.getPrototypeOf(new Proxy(rigid, { getPrototypeOf() { return base; } })) === base);
t("proto-free-open", () => Object.getPrototypeOf(new Proxy({}, { getPrototypeOf() { return base; } })) === base);

// ---- a forwarder pays for none of it --------------------------------------
const forward = new Proxy(frozen, {});
t("forward-get", () => forward.x);
t("forward-has", () => "x" in forward);
t("forward-keys", () => Object.keys(forward));
t("forward-own", () => own(forward, "x"));

// A revoked proxy is a TypeError at every internal method (10.5.1 step 2 and
// its siblings), which is a different rule from the invariants and is checked
// before any of them.
const pair = Proxy.revocable(frozen, { get() { return 1; } });
pair.revoke();
t("revoked-get", () => pair.proxy.x);
