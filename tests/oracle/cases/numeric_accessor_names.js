// A property name may be a NumericLiteral (13.2.5 LiteralPropertyName, 15.4
// ClassElementName), and that holds for accessors as much as for data and
// methods: `get 4()`, `set 2(v)`, `static get 6()`, and every numeric
// spelling — hex, octal, binary, separators, exponent, fraction, a BigInt —
// names the key its ToPropertyKey gives (`0x10` is "16", `1e21` is "1e+21",
// `1n` is "1"). The accessor's function is named "get <key>" / "set <key>"
// from that key, as a computed key's is (10.2.9 SetFunctionName). `get` and
// `set` are still plain names where no key follows them.
function show(label, f) {
  try { console.log(label, String(f())); } catch (e) { console.log(label, 'threw', e.constructor.name); }
}
const lit = {
  get 4() { return 'four'; },
  set 2(v) { this.two = v; },
  get 0x10() { return 'hex'; },
  get 0o17() { return 'oct'; },
  get 0b101() { return 'bin'; },
  get 1_000() { return 'sep'; },
  get 1e21() { return 'exp'; },
  get .5() { return 'half'; },
  get 2.50() { return 'frac'; },
  get 7n() { return 'big'; },
  set 7n(v) { this.bigSet = v; },
  get 0() { return 'zero'; },
  3: 'data3',
  9n: 'data9',
};
lit[2] = 'x';
lit[7] = 'y';
show('values', () => [lit[4], lit.two, lit[16], lit[15], lit[5], lit[1000], lit['1e+21'], lit['0.5'], lit['2.5'], lit[7], lit.bigSet, lit[0], lit[3], lit[9]].join());
show('keys', () => Object.keys(lit).join());
show('lit names', () => ['4', '16', '1e+21', '0.5', '7', '0'].map((k) => Object.getOwnPropertyDescriptor(lit, k).get.name).join('|'));
show('lit set names', () => ['2', '7'].map((k) => Object.getOwnPropertyDescriptor(lit, k).set.name).join('|'));
show('descriptor', () => { const d = Object.getOwnPropertyDescriptor(lit, '4'); return [typeof d.get, d.set, d.enumerable, d.configurable].join(); });

class C {
  get 4() { return 'c4'; }
  set 5(v) { this.v = v; }
  static get 6() { return 's6'; }
  static set 6(v) { C.s = v; }
  get 2n() { return 'c2'; }
  get 0x20() { return 'c32'; }
  static 8() { return 's8'; }
  9() { return 'm9'; }
  *10() { yield 'g10'; }
  async 11() { return 'a11'; }
  async *12() { yield 'ag12'; }
  13 = 'f13';
  static 14 = 's14';
  get() { return 'plain get'; }
  set(v) { return 'plain set ' + v; }
  static get get() { return 'static getter named get'; }
}
const ci = new C();
ci[5] = 9;
C[6] = 'sv';
show('class', () => [ci[4], ci.v, C[6], C.s, ci[2], ci[32], C[8](), ci[9](), ci[10]().next().value, ci[13], C[14]].join());
show('class plain', () => [ci.get(), ci.set(1), C.get].join());
show('class names', () => [Object.getOwnPropertyDescriptor(C.prototype, '4').get.name, Object.getOwnPropertyDescriptor(C.prototype, '5').set.name, Object.getOwnPropertyDescriptor(C, '6').get.name, Object.getOwnPropertyDescriptor(C, '6').set.name, Object.getOwnPropertyDescriptor(C.prototype, '2').get.name, Object.getOwnPropertyDescriptor(C.prototype, '32').get.name].join('|'));
show('class enumerable', () => Object.getOwnPropertyDescriptor(C.prototype, '4').enumerable);
show('class proto keys', () => Object.getOwnPropertyNames(C.prototype).join());
const k = 'x';
const sym = Symbol('tag');
const anon = Symbol();
const o2 = { get [k]() { return 1; }, set ['s'](v) {}, get [sym]() { return 2; }, get [anon]() { return 3; }, get [1 + 1]() { return 4; } };
show('computed names', () => [Object.getOwnPropertyDescriptor(o2, 'x').get.name, Object.getOwnPropertyDescriptor(o2, 's').set.name, Object.getOwnPropertyDescriptor(o2, sym).get.name, JSON.stringify(Object.getOwnPropertyDescriptor(o2, anon).get.name), Object.getOwnPropertyDescriptor(o2, '2').get.name].join('|'));
class D { get [k]() { return 1; } static [k + 'q']() {} [sym]() {} static set [anon](v) {} }
show('class computed names', () => [Object.getOwnPropertyDescriptor(D.prototype, 'x').get.name, D.xq.name, D.prototype[sym].name, JSON.stringify(Object.getOwnPropertyDescriptor(D, anon).set.name)].join('|'));
ci[11]().then((v) => console.log('async', v));
ci[12]().next().then((r) => console.log('async gen', r.value));
