// The write audit's SYNTACTIC ORACLE: what licenses a name to stay
// number-clean when the flow pass could only type one of its writes `dynamic`.
//
// `field_type_audit.js` pins the audit's write SET — that every write in the
// program counts, not only the ones the class body makes. This case pins the
// other half: what a single write is allowed to be believed about. A `true`
// there withdraws the refusal, the name keeps its primitive claim, and the read
// of that field lowers to `unbox.f64 ..., raw` — an unchecked reinterpretation
// of whatever bits the slot holds. So the only admissible argument is the FORM
// of the expression: `a * b` evaluates to a Number whichever bindings a and b
// are, and nothing about how a binding is SPELLED is evidence of anything.
//
// Every section below used to be believed on a spelling. A parameter called
// `x`, a property called `w`, a method called `dot`, a function called `lerp` —
// four hard-coded name lists that described three.js's conventions and were
// spent as proofs. `b.set('a', 1, 2); b.sum()` answered `NaN` where the
// language says `a12`.
//
// The reads are written as merges (a loop, an if/else) for the reason
// `field_type_audit.js` gives: a field type only becomes a hard unbox where
// lowering has to name a block parameter's type.

function say(label, v) {
  console.log(label + "=" + v + " (" + typeof v + ")");
}

// ---- 1. a parameter one call site passes a string to ---------------------
//
// The reported miscompile. Two call sites, so the parameter joins to `dynamic`;
// the class's own `this.x = 0` says `number`; the join of those is `dynamic`
// and the field is not a number field.

class V3 {
  constructor() {
    this.x = 0;
    this.y = 0;
    this.z = 0;
  }
  set(x, y, z) {
    this.x = x;
    this.y = y;
    this.z = z;
    return this;
  }
  sum() {
    let s = this.x + this.y + this.z;
    for (let i = 0; i < 2; i++) s = this.x + this.y + this.z;
    return s;
  }
}

const n1 = new V3();
n1.set(1, 2, 3);
say("numbers", n1.sum());
const s1 = new V3();
s1.set("a", 1, 2);
say("string first", s1.sum());

// The string site alone, so the parameter is `string` rather than `dynamic`
// and the refusal arrives by the ordinary road. It has to answer the same.
class Only {
  constructor() {
    this.only = 0;
  }
  set(only) {
    this.only = only;
    return this;
  }
  read() {
    let s = this.only;
    for (let i = 0; i < 2; i++) s = this.only;
    return s;
  }
}
const o1 = new Only();
o1.set("only");
say("only site", o1.read());

// No constructor write at all, so the field is installed by a method and the
// class layout claims no slot for it. A third road to the same answer.
class Late {
  set(x) {
    this.x = x;
    return this;
  }
  read() {
    let s = this.x;
    for (let i = 0; i < 2; i++) s = this.x;
    return s;
  }
}
const l1 = new Late();
l1.set(5);
const l2 = new Late();
l2.set("late");
say("late number", l1.read());
say("late string", l2.read());

// ---- 2. a source PROPERTY that is spelled like a number -----------------

class FromProp {
  constructor() {
    this.px = 0;
    this.py = 0;
  }
  copy(o) {
    this.px = o.w;
    this.py = 1;
    return this;
  }
  sum() {
    let s = this.px + this.py;
    for (let i = 0; i < 2; i++) s = this.px + this.py;
    return s;
  }
}
const w1 = new FromProp();
w1.copy({ w: 5 });
const w2 = new FromProp();
w2.copy({ w: "hi" });
say("from .w number", w1.sum());
say("from .w string", w2.sum());

// ---- 3. a source METHOD that is spelled like a number ------------------

const numDot = {
  dot() {
    return 5;
  }
};
const strDot = {
  dot() {
    return "hi";
  }
};
class FromMethod {
  constructor() {
    this.mx = 0;
    this.my = 0;
  }
  copy(o) {
    this.mx = o.dot();
    this.my = 1;
    return this;
  }
  sum() {
    let s = this.mx + this.my;
    for (let i = 0; i < 2; i++) s = this.mx + this.my;
    return s;
  }
}
const d1 = new FromMethod();
d1.copy(numDot);
const d2 = new FromMethod();
d2.copy(strDot);
say("from dot() number", d1.sum());
say("from dot() string", d2.sum());

// ---- 4. a source FUNCTION that is spelled like a number ----------------

function lerp(t) {
  return t;
}
class FromFunc {
  constructor() {
    this.fx = 0;
    this.fy = 0;
  }
  copy(t) {
    this.fx = lerp(t);
    this.fy = 1;
    return this;
  }
  sum() {
    let s = this.fx + this.fy;
    for (let i = 0; i < 2; i++) s = this.fx + this.fy;
    return s;
  }
}
const f1 = new FromFunc();
f1.copy(5);
const f2 = new FromFunc();
f2.copy("hi");
say("from lerp() number", f1.sum());
say("from lerp() string", f2.sum());

// ---- 5. warm first, then the store ------------------------------------
//
// The read site is monomorphic and hot before anything non-numeric reaches the
// slot, which is the arrangement an inline cache is most confident about.

class Warm {
  constructor() {
    this.wx = 1;
  }
  put(x) {
    this.wx = x;
  }
  read() {
    let s = this.wx;
    for (let i = 0; i < 2; i++) s = this.wx;
    return s;
  }
}
const wm = new Warm();
let acc = 0;
for (let i = 0; i < 200; i++) acc += wm.read();
say("warm sum", acc);
const other = new Warm();
other.put(2);
wm.put("cold");
say("after store", wm.read());
say("other instance", other.read());

// ---- 6. two instances through one read site ----------------------------
//
// Same shape, same site, one holding a number and one a string. The site
// cannot answer with a representation; it has to answer with the value.

class Pair {
  constructor() {
    this.value = 0;
  }
  set(value) {
    this.value = value;
    return this;
  }
}
function readValue(p) {
  let s = p.value;
  for (let i = 0; i < 2; i++) s = p.value;
  return s;
}
const pa = new Pair().set(7);
const pb = new Pair().set("seven");
say("pair a", readValue(pa));
say("pair b", readValue(pb));
say("pair a again", readValue(pa));

// ---- 7. the number path is unharmed, bit for bit -----------------------
//
// `n` is only ever written a Number, so it keeps its claim and every read below
// really is the raw unbox. What crosses it has to survive exactly: -0 is not 0,
// NaN is not equal to itself, and both infinities keep their sign.

class Num {
  constructor() {
    this.n = 0;
  }
  set(n) {
    this.n = n;
    return this;
  }
  read() {
    let s = this.n;
    for (let i = 0; i < 2; i++) s = this.n;
    return s;
  }
}
say("negative zero", 1 / new Num().set(-0).read());
say("positive zero", 1 / new Num().set(0).read());
const nan = new Num().set(NaN);
say("nan", nan.read());
say("nan self-equal", nan.read() === nan.read());
say("infinity", new Num().set(Infinity).read());
say("negative infinity", new Num().set(-Infinity).read());

// ---- 8. what a FORM still proves ---------------------------------------
//
// The operators that can only produce a Number keep the claim whatever reaches
// them, which is what separates a fact about the language from a fact about a
// naming convention. Every field here is written from a string and every read
// is still a number.

class Forms {
  constructor() {
    this.fa = 0;
    this.fb = 0;
    this.fc = 0;
    this.fd = 0;
  }
  fill(t) {
    this.fa = t * 2;
    this.fb = -t;
    this.fc = t | 0;
    this.fd = 3;
  }
  read() {
    let s = this.fa + this.fb + this.fc + this.fd;
    for (let i = 0; i < 2; i++) s = this.fa + this.fb + this.fc + this.fd;
    return s;
  }
}
const fm = new Forms();
fm.fill("4");
say("forms", fm.read());

// `+` is the one arithmetic operator that is not arithmetic: it concatenates as
// soon as either side is a string, so a number literal on the left proves
// nothing about the result.
class Plus {
  constructor() {
    this.p = 0;
  }
  fill(t) {
    this.p = 1 + t;
  }
  read() {
    let s = this.p;
    for (let i = 0; i < 2; i++) s = this.p;
    return s;
  }
}
const pl1 = new Plus();
pl1.fill(2);
say("plus number", pl1.read());
const pl2 = new Plus();
pl2.fill("x");
say("plus string", pl2.read());
