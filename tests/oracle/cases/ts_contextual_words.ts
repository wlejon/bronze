// In a TypeScript file the words that start TypeScript declarations are
// still ordinary identifiers wherever TypeScript itself reads them as one:
// `type`, `namespace`, `module`, `declare`, `abstract` and `interface` begin a
// declaration only when its name follows on the same line, so a line break
// after one ends the statement wherever ASI says it does. `as` and
// `satisfies` are operators only on the same line as their operand, and a
// `!` on the next line starts the next statement.
let type = 7, namespace = 2, declare = 3, abstract = 4, as = 5, satisfies = 6, module = 8;
console.log(type, namespace, declare, abstract, as, satisfies, module);

type
namespace = type + 1
console.log(namespace);

abstract
declare = abstract * 10
console.log(declare);

namespace
{
    console.log("a block after the identifier namespace");
}

module
module = 9
console.log(module);

let x = 1
as
console.log(x, as);
x
satisfies
console.log(x, satisfies);

let y = 0
!function () { console.log("negated call ran", y); }();

// The same words as property names and parameters.
const o = { type: 1, namespace: 2, declare: 3, abstract: 4, as: 5, enum: 6 };
function f(type: number, declare: number) { return type + declare; }
console.log(o.type + o.namespace + o.declare + o.abstract + o.as + o.enum, f(1, 2));

// Modifiers apply only to a member on the same line.
class Fields {
    public
    readonly = 2
    declare
    abstract = 3
}
const fields = new Fields();
console.log(Object.keys(fields).join(","), fields.readonly, fields.abstract);
