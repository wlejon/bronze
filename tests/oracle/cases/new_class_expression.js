// A class expression is a PrimaryExpression (13.2), so `new` constructs one
// written in place: model-viewer's `const wg = new class extends Nl { … }()`.
class Base { constructor(v) { this.v = v; } }
const o = new class extends Base { constructor() { super(42); } }();
const p = new class { constructor() { this.q = 1; } }();
const r = new (class extends Base {})(7);
function f() { return Base; }
const s = new class extends f() {}(9);
const t = new class Named { who() { return Named.name; } }().who();
console.log(o.v, p.q, r.v, s.v, t, o instanceof Base);
