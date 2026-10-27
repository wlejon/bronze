// A math alias read before its const runs is a TDZ read, not Math.cos.
function f(x) { return cos(x); }
class K { m(x) { return sin(x * 2); } }
try { console.log(f(0)); } catch (e) { console.log("f", e instanceof ReferenceError); }
try { console.log(new K().m(0)); } catch (e) { console.log("m", e instanceof ReferenceError); }
const cos = Math.cos;
const sin = Math.sin;
console.log(f(0), new K().m(0.25).toFixed(4));
