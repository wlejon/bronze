// A store of the receiver into itself inside what would be a transition run
// (a.self = a, this.me = this) ends the run: the run's stores are of values
// made before its one shape store, which the receiver is not.
function mk(i) { const a = {}; a.id = i; a.self = a; a.n = i * 2; a.pair = [a, 1]; return a; }
class C { constructor(i) { this.x = i; this.me = this; this.y = i + 1; this.z = this; } }
let s = 0;
for (let i = 0; i < 20000; i++) { const a = mk(i); const c = new C(i); s += a.self.self.n + (a.pair[0] === a) + c.me.z.y + (c.z === c); }
const a = mk(7); console.log(s, Object.keys(a).join(), Object.keys(new C(1)).join());
