// A `new` callee that is a MEMBER of an imported binding. The graph is one flat
// scope, so the base of the callee has to be renamed to the exporting file's
// binding — and getting that wrong is a silent wrong binding rather than a
// diagnostic, so it is pinned by VALUE.
//
// Derived from ECMA-262 16.2.1.6.4 (an import binding is an indirect
// reference to the exporting module's binding), 13.3.5 (the callee is a
// MemberExpression evaluated where it stands), and 9.1.1.4 / 14.3.1 (a
// function's own `const` shadows an outer binding for the whole body).
import { table as ta, which } from './a.js';
import { table as tb } from './b.js';

console.log(new ta.Ctor().tag);
console.log(new tb.Ctor().tag);

// The computed form: the INDEX is an ordinary expression, so an imported
// binding used as one has to be renamed too.
console.log(new ta[which]().tag);

// And the other direction: a function-local binding of the same name must
// NOT be renamed. A walk that rewrote every base it saw would print 'a'.
function shadowed() {
  const ta = tb;
  return new ta.Ctor().tag;
}
console.log(shadowed());
