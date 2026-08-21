// The write audit's computed-key path: `o[k] = v` and `delete o[k]`, whose name
// is decided at run time.
//
// The audit certifies "every property called `f` in this heap holds a Number",
// and that licenses a RAW f64 unbox — a bitcast with no tag test. A computed
// write is the one construct that can break the invariant under a name the pass
// never sees, so the only two ways past it are a KEY proven to be a Number
// (ToPropertyKey of one is a canonical numeric string, which no ordinary field
// is called) and a VALUE proven to be a Number (the invariant survives whatever
// name it lands on).
//
// Every line below is a way for one of those two proofs to be wrong: a key that
// is a string spelling a real field name, a key that is a number-LIKE string, a
// numeric key that reaches a field genuinely called "0", a value that is a
// number on one path and a string on another, a delete through a computed key,
// and a loop index used to write a field name built from it.

class Vec {
  constructor() {
    this.x = 1;
    this.y = 2;
    this.z = 3;
  }
  len() {
    return this.x + this.y + this.z;
  }
}

const v = new Vec();
console.log(v.len());

// A computed write with a proven-NUMBER key, on an array. This is the shape the
// audit has to let through: thousands of them in a real library, and none of
// them can name `x`, `y` or `z`.
const arr = [];
for (let i = 0; i < 5; i++) {
  arr[i] = "s" + i;
}
console.log(arr.join("|") + "," + v.len());

// The same loop index, but the key is a STRING built from it. It can name
// anything, and here it names a field the audit would otherwise have certified.
const names = ["x", "y", "z"];
for (let i = 0; i < names.length; i++) {
  v[names[i]] = "not a number";
}
console.log(v.x + v.y + v.z);
console.log(v.len());

// A field genuinely CALLED "0", written through a numeric key. The narrowing
// that lets `arr[i] = <anything>` past is exactly a claim about which names a
// numeric key can reach, and this is a name it reaches.
class Slots {
  constructor() {
    this["0"] = 10;
    this["1"] = 20;
  }
  sum() {
    return this["0"] + this["1"];
  }
}
const sl = new Slots();
console.log(sl.sum());
let k = 0;
sl[k] = "ten";
console.log(sl.sum() + "," + sl[0]);

// A computed write whose VALUE is a number on one path and not on another. The
// join is what the audit reads, and it has to be the wider of the two.
class Holder {
  constructor() {
    this.n = 1;
  }
}
const h = new Holder();
function put(target, key, flag) {
  target[key] = flag ? 42 : "forty-two";
}
put(h, "n", true);
console.log(h.n + 1);
put(h, "n", false);
console.log(h.n + 1);

// `delete` through a computed key. A deleted property reads `undefined`, which
// is not a Number, and the object drops out of its shape on the way.
class Deletable {
  constructor() {
    this.a = 1;
    this.b = 2;
  }
  both() {
    return this.a + "/" + this.b;
  }
}
const del = new Deletable();
console.log(del.both());
let key = "a";
delete del[key];
console.log(del.both());
console.log(typeof del.a);

// A computed delete through a NUMERIC key: it can only reach a numeric name, so
// it says nothing about `a` or `b`.
const del2 = new Deletable();
const idx = 0;
del2[idx] = 9;
delete del2[idx];
console.log(del2.both() + "," + typeof del2[0]);

// A compound assignment through a computed key: `13.15.3` reads, combines and
// stores, so what it stores is the combination and not the right-hand side.
class Acc {
  constructor() {
    this.t = 0;
  }
}
const acc = new Acc();
const field = "t";
for (let i = 1; i <= 4; i++) {
  acc[field] += i;
}
console.log(acc.t);
acc[field] += "!";
console.log(acc.t);

// A key that is a number-LIKE string. ToPropertyKey does not convert it — it is
// already a string — so `"1"` and `1` are the same key and `"01"` is not.
const q = {};
q[1] = "one";
q["1"] = "ONE";
q["01"] = "zero-one";
console.log(q[1] + "," + q["01"] + "," + Object.keys(q).length);

// A computed write to a name the program never writes anywhere else, on an
// object built by a literal. The object is not an instance of anything the
// analysis models, and the name it lands on is still a name.
const fresh = {};
const dyn = "x";
fresh[dyn] = "a string under x";
const v2 = new Vec();
console.log(fresh.x + "," + v2.x);

// Warm: a hot loop writing a proven-number key beside reads of a field the
// audit would otherwise certify. Nothing here may unbox a string as a double.
const buf = [];
const v3 = new Vec();
let sum = 0;
for (let i = 0; i < 400; i++) {
  buf[i] = i * 2;
  sum += v3.x + buf[i];
}
console.log(sum + "," + buf[399]);
