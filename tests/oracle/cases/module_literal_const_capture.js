// A module-scope object literal read from inside a FUNCTION rather than from
// the top level.
//
// The other `module_literal_*` cases ask their questions where the literal is
// declared. This one asks them from every position a function body can be: a
// closure, a class method, a parameter default, an arrow nested inside a
// method, a loop, and a hoisted `function` declaration. A read written at top
// level and the same read written inside a closure are one expression to the
// language and two different things to a compiler — the second reaches
// whatever it reads through the record the module scope keeps, and that
// record's layout is fixed before any body is lowered. Every answer below must
// be the same in all six positions, so a compiler that resolved one of them
// against a different binding, or against no binding at all, prints two
// different values for one property on adjacent lines.
//
// `both` is the same question doubled: it reads `Fold.space` AND the binding
// `SPACE` by name in one expression, so the binding is one the closure
// genuinely captures. `hoisted` is the position with the extra hazard — 8.6.2
// instantiates a top-level `function` declaration before the first statement
// runs, so its body can be entered while `Fold` is still uninitialized, where
// 13.3.2's read of `Fold` is the ReferenceError the dead zone gives. Called
// after the declaration, as it is here, it must answer exactly what the other
// five spellings answer.

const SPACE = 'srgb-linear';
const COUNT = 4;

const Fold = {
  _space: SPACE,
  size: COUNT,
  flag: false,
  none: null,
  gone: undefined,
  get space() { return this._space; },
  // Never entered: 10.1.9.1 calls a setter only from a [[Set]] of its own key,
  // and nothing in this program writes `space`.
  set space(v) { this._space = v; }
};

// A closure created below the declaration. Its body runs no earlier than the
// statement that builds it, which is below the literal, so `Fold` is always
// initialized by the time this reads it.
const readSpace = function () { return Fold.space; };

// The binding read by name and through the property, in one expression.
const both = function () { return Fold.space + '|' + SPACE + '|' + Fold['_space']; };

class Shader {
  constructor(name) { this.name = name; }
  // A parameter default is evaluated per call and only when the argument is
  // `undefined` (10.2.11 / 8.6.3) — the three.js shape exactly.
  describe(cs = Fold.space) { return this.name + ':' + cs + ':' + Fold.size; }
  nested() {
    // Further from the module record than any other read here: the arrow's own
    // scope, then the method's, then the module's.
    const f = () => Fold.space + '#' + Fold.flag;
    return f();
  }
}

function hoisted() { return Fold.space + '~' + Fold.size; }

console.log('closure       ' + readSpace());
console.log('both          ' + both());
// 7.2.16: two Strings are strictly equal when their code units are.
console.log('identity      ' + (readSpace() === SPACE));

const s = new Shader('lit');
console.log('method deflt  ' + s.describe());
console.log('method arg    ' + s.describe('display-p3'));
console.log('nested arrow  ' + s.nested());
console.log('hoisted       ' + hoisted());

// 13.5.3: `typeof null` is "object", and an own property holding `undefined`
// is still an own property whose value is `undefined`.
console.log('kinds         ' + Fold.size + ' ' + Fold.flag + ' ' + Fold.none + ' ' + Fold.gone);
console.log('kind types    ' + typeof Fold.size + ' ' + typeof Fold.flag + ' ' +
            typeof Fold.none + ' ' + typeof Fold.gone);

// The same reads a thousand times over, through a call each time. An answer
// that drifts is a constant that outlived its proof.
let hot = 0;
for (let i = 0; i < 1000; i++) {
  if (readSpace() === 'srgb-linear') hot += Fold.size;
}
console.log('hot           ' + hot);
