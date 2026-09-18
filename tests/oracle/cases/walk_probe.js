function names(s, count) {
  const all = (s.match(/at (\S+)/g) || []).map(x => {
    let name = x.slice(3);
    if (name.includes(':') || name === 'Object.<anonymous>') return '<arrow>';
    return name;
  });
  return (count ? all.slice(0, count) : all).join(',');
}

function lineNumbers(s, count) {
  const lines = s.split('\n').slice(1);
  const nums = [];
  for (const l of lines) {
    const m = l.match(/:(\d+):\d+\)?$/);
    if (m) nums.push(Number(m[1]));
  }
  return (count ? nums.slice(0, count) : nums).join(',');
}

function mk() { return new Error("x"); }
function a1() { return mk(); }
function a2() { return a1(); }
function a3() { return a2(); }
class K { m() { return mk(); } n() { return this.m(); } }
function viaForEach() { let e; [1].forEach(() => { e = mk(); }); return e; }
function thrower() { throw new RangeError("r"); }
function catcher() { try { thrower(); } catch (e) { return e; } }
function rt() { const u = undefined; return u.x; }
function rtc() { try { rt(); } catch (e) { return e; } }

console.log(names(a3().stack, 4));
console.log(names(new K().n().stack, 3));
console.log(names(viaForEach().stack, 4));
console.log(names(catcher().stack, 2));
console.log(names(rtc().stack, 2));
console.log(lineNumbers(a3().stack, 4));
