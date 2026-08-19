// 20.2.3.5 Function.prototype.toString: a function that HAS source text
// returns that text, verbatim, and the result is expected to parse back into
// an equivalent function. Library code reads it — argument names for
// dependency injection, `class ` off the front of a constructor, and the
// hook-identity checks that compare a function's text against the original.
// `[native code]` for all of them was a wrong answer none of those callers
// could detect, because it is also the RIGHT answer for a builtin.
//
// The text is the source matched by the production, so it starts at
// `function`/`get`/`async`/`*` and ends at the closing brace — not at the next
// token, which is why a trailing comment is not part of it.
function add(a, b) { return a + b; }   /* not part of the text */
console.log(add.toString());

const arrow = (x) => x * 2;
console.log(arrow.toString());
const bare = x => x + 1;
console.log(bare.toString());

const anon = function (q) { return q; };
console.log(anon.toString());

// A class's text is the whole class (15.7.14 makes the class's source text
// the constructor's), which is what `toString().startsWith("class")` asks.
class Shape { constructor(n) { this.n = n; } area() { return 0; } }
console.log(Shape.toString());
console.log(Shape.prototype.area.toString());

// The keyword that selects the form is part of the text.
const forms = {
  meth(z) { return z; },
  *gm() { yield 1; },
  async am() { return 1; },
  async *agm() { yield 1; },
  get gg() { return 1; },
  set ss(v) { },
};
console.log(forms.meth.toString());
console.log(forms.gm.toString());
console.log(forms.am.toString());
console.log(forms.agm.toString());
console.log(Object.getOwnPropertyDescriptor(forms, "gg").get.toString());
console.log(Object.getOwnPropertyDescriptor(forms, "ss").set.toString());

function* gen(i) { yield i; }
async function aw(v) { return v; }
console.log(gen.toString());
console.log(aw.toString());

// A nested function's text is its own, and the enclosing one's includes it.
function outer() { return function inner(y) { return y; }; }
console.log(outer.toString());
console.log(outer().toString());

// Every closure over one body shares that body's text.
function makeAll() {
  const out = [];
  for (let i = 0; i < 3; i++) out.push(() => i);
  return out;
}
const closures = makeAll();
console.log(closures[0].toString(), closures[0].toString() === closures[2].toString());

// A NATIVE function has no source text, and the NativeFunction string is the
// correct answer for it — as it is for a bound function (10.4.1 gives the
// exotic object no [[SourceText]]).
// 20.2.3.5's NativeFunction production requires the body to be exactly
// `[native code]`; the name in front of it is optional, which is why this
// asks about the part the spec pins.
console.log(Math.max.toString().includes("[native code]"));
console.log(add.bind(null).toString());

// The receiver must be callable (step 1 of 20.2.3.5).
try { Function.prototype.toString.call(123); } catch (e) { console.log(e.name); }
try { Function.prototype.toString.call(null); } catch (e) { console.log(e.name); }

// The text really does parse back: it is the bytes, not a rendering.
console.log(add.toString() === "function add(a, b) { return a + b; }");
