// A TypeScript program across modules. `import type` and inline `type`
// specifiers name nothing at run time and are erased; `./shapes.js` names a
// file that does not exist, and resolves to `./shapes.ts` beside it, the
// TypeScript ESM convention for writing the specifier the compiled output
// will use. (node does not resolve `.js` to `.ts`; this case's expectation was
// generated from the same program importing "./shapes.ts", which is the only
// difference.)
import type { Foo } from "./types.ts";
import { type Bar, makeBar } from "./types.ts";
import { Kind, Area, Shape } from "./shapes.js";

const foo: Foo = { n: 1 };
const bar: Bar = makeBar();
console.log("values", foo.n, bar.v);

const shapes = [new Shape(Kind.Circle, 2), new Shape(Kind.Square, 3)];
for (const s of shapes) console.log(s.kind, s.area(), s.dims().join("x"));
console.log("live namespace export across modules", Area.calls);
Area.of(Kind.Square, 1);
console.log("after another call", Area.calls);
