// Guarded method inlining and `+` chains: a resolved getter or method called
// between a chain's links (`a.x + ':' + a.m() + ':'`) in a loop stays a call,
// since a body copied there would split the block between the chain's links
// (lower/inline_guarded.cpp). The chain's value is the same whichever way each
// call goes: the guarded body, the real call, a replaced method, an own
// property shadowing it, a receiver of another class, a throwing getter.

class Hash {
  constructor(a, f) { this.a = a; this.f = f; this.count = 0; }
  get exact() { return (this.a >>> 0).toString(16).padStart(8, '0'); }
  get f32() { return (this.f >>> 0).toString(16).padStart(8, '0'); }
  get hexA() { return (this.a >>> 0).toString(16); }
  tag(n) { return 't' + n; }
  depth(n) { return n <= 0 ? '.' : '(' + this.depth(n - 1) + ')'; }
}

class Other {
  get exact() { return 'other'; }
  get f32() { return 'o32'; }
  tag(n) { return 'o' + n; }
  depth(n) { return '#' + n; }
}

function run(hs) {
  const out = [];
  for (let i = 0; i < hs.length; i++) {
    const h = hs[i];
    out.push(h.exact + ':' + h.f32 + ':' + h.tag(i) + ':' + h.depth(i % 3) + ':' + i);
  }
  return out.join(',');
}

// A getter of one class that no write redefines, read from an array only that
// class's instances reach: its sites resolve, and are copied in wherever they
// are not between a chain's links.
function plain(hx) {
  const out = [];
  for (let i = 0; i < hx.length; i++) out.push(hx[i].hexA + ':' + hx[i].hexA.length + ':' + i);
  return out.join(',');
}

function guarded(hs) {
  const out = [];
  for (let i = 0; i < hs.length; i++) {
    try {
      out.push('[' + hs[i].exact + '|' + hs[i].tag(i) + ']');
    } catch (e) {
      out.push('!' + e.message);
    }
  }
  return out.join('');
}

const hs = [];
for (let i = 0; i < 6; i++) hs.push(new Hash(i * 977, i * 31));
console.log(run(hs));
const hx = [new Hash(11, 1), new Hash(4095, 2), new Hash(65536, 3)];
console.log(plain(hx));

// Receivers of another class between ours.
hs[2] = new Other();
console.log(run(hs));

// An own property shadowing the prototype's method.
const shadow = new Hash(5, 6);
shadow.tag = function (n) { return 'own' + n; };
hs[3] = shadow;
console.log(run(hs));

// The method replaced on the prototype, then a getter replaced too.
Hash.prototype.tag = function (n) { return 'new' + n; };
console.log(run(hs));
Object.defineProperty(Hash.prototype, 'exact', { get() { return 'X' + this.a; }, configurable: true });
console.log(run(hs));

// A getter that throws inside the chain, caught per element.
const bad = new Hash(1, 2);
Object.defineProperty(bad, 'exact', { get() { throw new Error('boom' + this.a); } });
hs[4] = bad;
console.log(guarded(hs));
try {
  run(hs);
} catch (e) {
  console.log('caught', e.message);
}

// Many rounds, so a compiled loop's guards are what run.
let total = 0;
const fresh = [new Hash(7, 8), new Hash(9, 10), new Other()];
for (let r = 0; r < 2000; r++) total += run(fresh).length + plain(hx).length;
console.log(total);
