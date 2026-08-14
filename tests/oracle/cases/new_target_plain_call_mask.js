// Pins ECMA-262 9.4.5 / 10.2.1.1: new.target inside a PLAIN call is
// undefined even when the call happens during an active construction —
// [[Construct]] binds NewTarget for the constructor's own frame only, and
// [[Call]] binds it to undefined. In bronze the mask is the
// NewTargetScope(undefined) push in bronze_dynamic_call; any call fast path
// that skips that push must be disabled for a module mentioning new.target
// (the same module-wide rule the inline `new` path follows). This case
// exists because chunk 10's inline dynamic call shipped without the gate and
// leaked the enclosing constructor into a plain callee.
function probe() { return new.target; }
function mid(f) { return f(); }
function C(f) { this.got = mid(f); }
const c = new C(probe);
console.log(c.got === undefined); // the mask: plain call, no NewTarget
console.log(c.got === C);         // the leak this pins against
// The mask must not damage the real thing: a constructor still sees itself,
// through the same dynamic-call-shaped sites at loop heat.
function D() { this.viaNew = new.target === D; }
let all = true;
for (let i = 0; i < 200; i = i + 1) {
  all = all && new D().viaNew;
}
console.log(all);
// And a plain call at top level, where the scope stack is empty.
console.log(probe() === undefined);
