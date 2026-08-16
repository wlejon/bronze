// Private class elements (ECMA-262 6.2.12): instance fields, private methods
// and private accessors, and the brand check that guards every access.
//
// A private name is not a property key. It is an entry in the object's
// [[PrivateElements]] list, so no enumeration can see it, and `#x` on an object
// whose class did not declare it is a TypeError rather than `undefined` — the
// difference between a private field and a property named oddly.
//
// The three TypeErrors a BRANDED access can still be are here too (6.2.12.2
// step 3.b, 6.2.12.3 steps 2 and 4): writing a private method, reading an
// accessor with no getter, writing one with no setter.

class Vault {
  #code = 1234;
  #log = [];

  #record(what) {
    this.#log.push(what);
    return this.#log.length;
  }

  get #secret() {
    this.#record("get");
    return this.#code;
  }

  set #secret(v) {
    this.#record("set");
    this.#code = v;
  }

  get #readOnly() {
    return "ro";
  }

  set #writeOnly(v) {
    this.#code = v;
  }

  read() {
    return this.#secret;
  }

  write(v) {
    this.#secret = v;
    return this.#code;
  }

  entries() {
    return this.#log.join(",");
  }

  bump() {
    return this.#code++;
  }

  recorder() {
    return this.#record;
  }

  static peek(o) {
    return o.#code;
  }

  static writeMethod(o) {
    o.#record = 1;
  }

  static readWriteOnly(o) {
    return o.#writeOnly;
  }

  static writeReadOnly(o) {
    o.#readOnly = 1;
  }
}

const v = new Vault();
// The accessor pair runs the private method, so the log is evidence of which
// half ran and in what order.
console.log(v.read());
console.log(v.write(7));
console.log(v.read());
console.log(v.entries());
// `this.#code++` reaches the FIELD, not the accessor, so it adds no log entry
// and yields the old value.
console.log(v.bump());
console.log(v.read());
console.log(v.entries());

// Invisible to every enumeration there is.
console.log(Object.keys(v).length);
console.log(Object.getOwnPropertyNames(v).length);
console.log(JSON.stringify(v));

// A private method is ONE function object per class evaluation, shared by every
// instance — it is stored with the brand, not copied into each object.
const other = new Vault();
console.log(v.recorder() === other.recorder());

function show(fn) {
  try {
    fn();
    console.log("no throw");
  } catch (e) {
    console.log(e.name + ": " + e.message);
  }
}

// The brand check, from outside and from a receiver of the wrong kind. The
// constructor is not an instance, so it carries no instance element either.
show(function () { return Vault.peek({}); });
show(function () { return Vault.peek("string"); });
show(function () { return Vault.peek(Vault); });
// Branded, and still a TypeError: the kind of the element decides.
show(function () { return Vault.writeMethod(v); });
show(function () { return Vault.readWriteOnly(v); });
show(function () { return Vault.writeReadOnly(v); });

// A subclass instance carries the base's private elements, because the base
// constructor is what installs them and `super()` runs it on this object.
class Sub extends Vault {
  peekOwn() {
    return Vault.peek(this);
  }
}

const s = new Sub();
console.log(s.read());
console.log(s.peekOwn());
console.log(s.entries());
console.log(s instanceof Vault);
