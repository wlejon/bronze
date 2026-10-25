// The words TypeScript reads as the start of a declaration or as an operator
// are ordinary identifiers in a .js file: `type`, `namespace`, `module`,
// `declare`, `abstract`, `interface`, `as`, `satisfies`, `readonly`,
// `public`. bronze reads TypeScript natively; in a .js file none of them may
// mean anything but a name, and a line break after one ends the statement
// wherever ASI says it does (ECMA-262 12.10).
let type = 7, namespace = 2, declare = 3, abstract = 4, as = 5, satisfies = 6;
console.log(type, namespace, declare, abstract, as, satisfies);

// A word followed by a line break and then what would be its declaration.
type
namespace = type + 1
console.log(namespace);

declare
abstract = declare * 10
console.log(abstract);

function interfaceOf(x) { return typeof x; }
const interface_ = interfaceOf(type);
console.log(interface_);

// `as` / `satisfies` after a value and a line break: two statements.
let x = 1
as
console.log(x, as);
x
satisfies
console.log(x, satisfies);

// A `!` on the next line is the start of the next statement, never a
// non-null assertion or a definite-assignment mark.
let y = 0
!function () { console.log("negated call ran"); }();
let z
!function () { console.log("second negated call ran", z); }();

// Class members named like TypeScript modifiers. With a line break after the
// word, it is a field of that name and the next line another field.
class Fields {
    public
    readonly = 2
    static
    declare = 3
    abstract() { return "method named abstract"; }
}
const f = new Fields();
console.log(Object.keys(f).join(","), f.public, f.readonly, Fields.declare, f.abstract());

// `module` and `namespace` as objects with members.
const module_ = { exports: { k: 1 } };
const ns = { namespace: { a: 1 } };
console.log(module_.exports.k, ns.namespace.a);

// A parenthesized middle of a conditional, followed by an arrow in the
// alternative: the parentheses hold an expression, and the colon is the
// conditional's, whatever is in them.
const cond = true, w = 4;
console.log(cond ? (w) : (q) => q);
console.log(typeof (false ? (w) : (q) => q * 2));
let assigned;
console.log(cond ? (assigned = 1) : (q) => q, assigned);
console.log((false ? (w, 1) : (q) => q + 1)(9));

// A case clause's colon is the same hazard.
function pick(k) {
    switch (k) {
        case (w): return "four";
        default: return "other";
    }
}
console.log(pick(4), pick(5));
