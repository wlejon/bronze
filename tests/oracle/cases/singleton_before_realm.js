// A ReferenceError for an unresolved name makes the thread's default realm
// lazily. Function singletons made before it must stay the same objects after:
// a declared function's identity cannot change across the throw.
function L() {}
function f() { return L; }
const before = f();
try { x; } catch {}
console.log(f() === L, before === L, before === f());
L.tag = 7;
console.log(f().tag);
class C { m() { return 1; } }
const c1 = C;
try { undefinedName(); } catch (e) { console.log(e instanceof ReferenceError); }
console.log(C === c1, new C() instanceof c1);
