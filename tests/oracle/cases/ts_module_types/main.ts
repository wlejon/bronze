// Type names resolve per module, as values do. Each of ids_a and ids_b has a
// private `type Id` of a different shape, and this module has a third; each
// annotation means its own module's `Id`. Exported types reach an importer
// only through an import: `import type`, an inline `type` specifier, or an
// ordinary import of a class.
import type { Account } from "./ids_a.ts";
import { makeAccount, nextId } from "./ids_a.ts";
import { Label, concatIds, type Tag as LabelTag } from "./ids_b.ts";

type Id = boolean[];

const flags: Id = [true, false];
const account: Account = makeAccount("ada");
const label: Label = new Label("x");
const tag: LabelTag = label.tag();

console.log(flags.length, flags[0]);
console.log(account.id, account.name, nextId(4));
console.log(concatIds("a", "b"), concatIds("1", "2") + 3);
console.log(tag, label instanceof Label);

// A block's own `Id` shadows the module's for the block alone.
{
    type Id = number;
    const n: Id = 5;
    console.log(n + 1);
}
const again: Id = [false];
console.log(again.length);
