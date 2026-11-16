// Double slots (runtime/slot_repr.h, docs/slot-representation.md): each
// writer below trains its own key on Numbers (the harvest makes the key
// eligible and the build reads its slot as a Number behind the shape test),
// then writes non-Numbers late. Every writer must move the object off the
// double shape (or never put it there), so the trained reads see the value
// written. The late objects get a Number back before the run ends, so the
// harvest the profile build reads still calls the keys Numbers.
// Generated (one key, class and read site per writer).
const show = (v) => typeof v === 'number' ? (Object.is(v, -0) ? '-0' : String(v)) : typeof v + ':' + String(v);
function keyOf(k) { return k; }
const TRAIN = 0.25, TRAIN2 = 0.125;
class C_assign { constructor(a, b) { this.k_assign = a; this.y = b; } }
function make_assign(v) { const o = new C_assign(1.5, 2); o.k_assign = v; return o; }
function read_assign(o) { return o.k_assign; }
function sum_assign(o) { return o.k_assign * 2 + o.y; }
function restore_assign(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_assign') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_assign') && Object.getOwnPropertyDescriptor(o, 'k_assign').writable) o.k_assign = 0.5; }
class C_assignCold { constructor(a, b) { this.k_assignCold = a; this.y = b; } }
function make_assignCold(v) { const o = new C_assignCold(1.5, 2); if (v === TRAIN) return o; o.k_assignCold = v; return o; }
function read_assignCold(o) { return o.k_assignCold; }
function sum_assignCold(o) { return o.k_assignCold * 2 + o.y; }
function restore_assignCold(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_assignCold') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_assignCold') && Object.getOwnPropertyDescriptor(o, 'k_assignCold').writable) o.k_assignCold = 0.5; }
class C_compound { constructor(a, b) { this.k_compound = a; this.y = b; } }
function make_compound(v) { const o = new C_compound(1.5, 2); o.k_compound += (typeof v === "number" ? v : v); return o; }
function read_compound(o) { return o.k_compound; }
function sum_compound(o) { return o.k_compound * 2 + o.y; }
function restore_compound(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_compound') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_compound') && Object.getOwnPropertyDescriptor(o, 'k_compound').writable) o.k_compound = 0.5; }
class C_keyed { constructor(a, b) { this.k_keyed = a; this.y = b; } }
function make_keyed(v) { const o = new C_keyed(1.5, 2); o[keyOf("k_keyed")] = v; return o; }
function read_keyed(o) { return o.k_keyed; }
function sum_keyed(o) { return o.k_keyed * 2 + o.y; }
function restore_keyed(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_keyed') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_keyed') && Object.getOwnPropertyDescriptor(o, 'k_keyed').writable) o.k_keyed = 0.5; }
class C_define { constructor(a, b) { this.k_define = a; this.y = b; } }
function make_define(v) { const o = new C_define(1.5, 2); Object.defineProperty(o, "k_define", { value: v, writable: true, enumerable: true, configurable: true }); return o; }
function read_define(o) { return o.k_define; }
function sum_define(o) { return o.k_define * 2 + o.y; }
function restore_define(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_define') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_define') && Object.getOwnPropertyDescriptor(o, 'k_define').writable) o.k_define = 0.5; }
class C_defineValue { constructor(a, b) { this.k_defineValue = a; this.y = b; } }
function make_defineValue(v) { const o = new C_defineValue(1.5, 2); Object.defineProperty(o, "k_defineValue", { value: v }); return o; }
function read_defineValue(o) { return o.k_defineValue; }
function sum_defineValue(o) { return o.k_defineValue * 2 + o.y; }
function restore_defineValue(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_defineValue') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_defineValue') && Object.getOwnPropertyDescriptor(o, 'k_defineValue').writable) o.k_defineValue = 0.5; }
class C_getter { constructor(a, b) { this.k_getter = a; this.y = b; } }
function make_getter(v) { const o = new C_getter(1.5, 2); if (typeof v !== "number") Object.defineProperty(o, "k_getter", { get() { return v; }, configurable: true, enumerable: true }); else o.k_getter = v; return o; }
function read_getter(o) { return o.k_getter; }
function sum_getter(o) { return o.k_getter * 2 + o.y; }
function restore_getter(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_getter') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_getter') && Object.getOwnPropertyDescriptor(o, 'k_getter').writable) o.k_getter = 0.5; }
class C_reflect { constructor(a, b) { this.k_reflect = a; this.y = b; } }
function make_reflect(v) { const o = new C_reflect(1.5, 2); Reflect.set(o, "k_reflect", v); return o; }
function read_reflect(o) { return o.k_reflect; }
function sum_reflect(o) { return o.k_reflect * 2 + o.y; }
function restore_reflect(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_reflect') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_reflect') && Object.getOwnPropertyDescriptor(o, 'k_reflect').writable) o.k_reflect = 0.5; }
class C_assignAll { constructor(a, b) { this.k_assignAll = a; this.y = b; } }
function make_assignAll(v) { const o = new C_assignAll(1.5, 2); Object.assign(o, { k_assignAll: v }); return o; }
function read_assignAll(o) { return o.k_assignAll; }
function sum_assignAll(o) { return o.k_assignAll * 2 + o.y; }
function restore_assignAll(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_assignAll') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_assignAll') && Object.getOwnPropertyDescriptor(o, 'k_assignAll').writable) o.k_assignAll = 0.5; }
class C_spreadOver { constructor(a, b) { this.k_spreadOver = a; this.y = b; } }
function make_spreadOver(v) { const o = new C_spreadOver(1.5, 2); return { ...o, k_spreadOver: v }; }
function read_spreadOver(o) { return o.k_spreadOver; }
function sum_spreadOver(o) { return o.k_spreadOver * 2 + o.y; }
function restore_spreadOver(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_spreadOver') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_spreadOver') && Object.getOwnPropertyDescriptor(o, 'k_spreadOver').writable) o.k_spreadOver = 0.5; }
class C_spreadFrom { constructor(a, b) { this.k_spreadFrom = a; this.y = b; } }
function make_spreadFrom(v) { const s = { k_spreadFrom: v, y: 2 }; const r = { ...s }; s.k_spreadFrom = 0.5; return r; }
function read_spreadFrom(o) { return o.k_spreadFrom; }
function sum_spreadFrom(o) { return o.k_spreadFrom * 2 + o.y; }
function restore_spreadFrom(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_spreadFrom') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_spreadFrom') && Object.getOwnPropertyDescriptor(o, 'k_spreadFrom').writable) o.k_spreadFrom = 0.5; }
class C_literal { constructor(a, b) { this.k_literal = a; this.y = b; } }
function make_literal(v) { return { k_literal: v, y: 2 }; }
function read_literal(o) { return o.k_literal; }
function sum_literal(o) { return o.k_literal * 2 + o.y; }
function restore_literal(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_literal') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_literal') && Object.getOwnPropertyDescriptor(o, 'k_literal').writable) o.k_literal = 0.5; }
class C_literalLate { constructor(a, b) { this.k_literalLate = a; this.y = b; } }
function make_literalLate(v) { return { y: 2, k_literalLate: v }; }
function read_literalLate(o) { return o.k_literalLate; }
function sum_literalLate(o) { return o.k_literalLate * 2 + o.y; }
function restore_literalLate(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_literalLate') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_literalLate') && Object.getOwnPropertyDescriptor(o, 'k_literalLate').writable) o.k_literalLate = 0.5; }
class C_json { constructor(a, b) { this.k_json = a; this.y = b; } }
function make_json(v) { return JSON.parse(JSON.stringify({ k_json: v === undefined ? null : v, y: 2 })); }
function read_json(o) { return o.k_json; }
function sum_json(o) { return o.k_json * 2 + o.y; }
function restore_json(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_json') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_json') && Object.getOwnPropertyDescriptor(o, 'k_json').writable) o.k_json = 0.5; }
class C_fromEntries { constructor(a, b) { this.k_fromEntries = a; this.y = b; } }
function make_fromEntries(v) { return Object.fromEntries([["k_fromEntries", v], ["y", 2]]); }
function read_fromEntries(o) { return o.k_fromEntries; }
function sum_fromEntries(o) { return o.k_fromEntries * 2 + o.y; }
function restore_fromEntries(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_fromEntries') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_fromEntries') && Object.getOwnPropertyDescriptor(o, 'k_fromEntries').writable) o.k_fromEntries = 0.5; }
class C_create { constructor(a, b) { this.k_create = a; this.y = b; } }
function make_create(v) { return Object.create(Object.prototype, { k_create: { value: v, writable: true, enumerable: true, configurable: true }, y: { value: 2, writable: true, enumerable: true, configurable: true } }); }
function read_create(o) { return o.k_create; }
function sum_create(o) { return o.k_create * 2 + o.y; }
function restore_create(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_create') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_create') && Object.getOwnPropertyDescriptor(o, 'k_create').writable) o.k_create = 0.5; }
class C_ctor { constructor(a, b) { this.k_ctor = a; this.y = b; } }
function make_ctor(v) { return new C_ctor(v, 2); }
function read_ctor(o) { return o.k_ctor; }
function sum_ctor(o) { return o.k_ctor * 2 + o.y; }
function restore_ctor(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_ctor') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_ctor') && Object.getOwnPropertyDescriptor(o, 'k_ctor').writable) o.k_ctor = 0.5; }
class C_field { constructor(a, b) { this.k_field = a; this.y = b; } }
let FIELD_field = 0;
class F_field { k_field = FIELD_field; y = 2; }
function make_field(v) { FIELD_field = v; return new F_field(); }
function read_field(o) { return o.k_field; }
function sum_field(o) { return o.k_field * 2 + o.y; }
function restore_field(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_field') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_field') && Object.getOwnPropertyDescriptor(o, 'k_field').writable) o.k_field = 0.5; }
class C_deleteReadd { constructor(a, b) { this.k_deleteReadd = a; this.y = b; } }
function make_deleteReadd(v) { const o = new C_deleteReadd(1.5, 2); delete o.k_deleteReadd; o.k_deleteReadd = v; return o; }
function read_deleteReadd(o) { return o.k_deleteReadd; }
function sum_deleteReadd(o) { return o.k_deleteReadd * 2 + o.y; }
function restore_deleteReadd(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_deleteReadd') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_deleteReadd') && Object.getOwnPropertyDescriptor(o, 'k_deleteReadd').writable) o.k_deleteReadd = 0.5; }
class C_freeze { constructor(a, b) { this.k_freeze = a; this.y = b; } }
function make_freeze(v) { const o = new C_freeze(1.5, 2); o.k_freeze = typeof v === "number" ? v : 7.5; Object.freeze(o); o.k_freeze = v; return o; }
function read_freeze(o) { return o.k_freeze; }
function sum_freeze(o) { return o.k_freeze * 2 + o.y; }
function restore_freeze(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_freeze') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_freeze') && Object.getOwnPropertyDescriptor(o, 'k_freeze').writable) o.k_freeze = 0.5; }
class C_destructure { constructor(a, b) { this.k_destructure = a; this.y = b; } }
function make_destructure(v) { const o = new C_destructure(1.5, 2); [o.k_destructure] = [v]; return o; }
function read_destructure(o) { return o.k_destructure; }
function sum_destructure(o) { return o.k_destructure * 2 + o.y; }
function restore_destructure(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_destructure') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_destructure') && Object.getOwnPropertyDescriptor(o, 'k_destructure').writable) o.k_destructure = 0.5; }
class C_destructObj { constructor(a, b) { this.k_destructObj = a; this.y = b; } }
function make_destructObj(v) { const o = new C_destructObj(1.5, 2); ({ q: o.k_destructObj } = { q: v }); return o; }
function read_destructObj(o) { return o.k_destructObj; }
function sum_destructObj(o) { return o.k_destructObj * 2 + o.y; }
function restore_destructObj(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_destructObj') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_destructObj') && Object.getOwnPropertyDescriptor(o, 'k_destructObj').writable) o.k_destructObj = 0.5; }
class C_forOf { constructor(a, b) { this.k_forOf = a; this.y = b; } }
function make_forOf(v) { const o = new C_forOf(1.5, 2); for (o.k_forOf of [v]); return o; }
function read_forOf(o) { return o.k_forOf; }
function sum_forOf(o) { return o.k_forOf * 2 + o.y; }
function restore_forOf(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_forOf') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_forOf') && Object.getOwnPropertyDescriptor(o, 'k_forOf').writable) o.k_forOf = 0.5; }
class C_increment { constructor(a, b) { this.k_increment = a; this.y = b; } }
function make_increment(v) { const o = new C_increment(1.5, 2); o.k_increment = v; o.k_increment++; return o; }
function read_increment(o) { return o.k_increment; }
function sum_increment(o) { return o.k_increment * 2 + o.y; }
function restore_increment(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_increment') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_increment') && Object.getOwnPropertyDescriptor(o, 'k_increment').writable) o.k_increment = 0.5; }
class C_int32 { constructor(a, b) { this.k_int32 = a; this.y = b; } }
function make_int32(v) { const o = new C_int32(1.5, 2); o.k_int32 = (typeof v === "number" ? v : 9) | 0; o.k_int32 = v; return o; }
function read_int32(o) { return o.k_int32; }
function sum_int32(o) { return o.k_int32 * 2 + o.y; }
function restore_int32(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_int32') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_int32') && Object.getOwnPropertyDescriptor(o, 'k_int32').writable) o.k_int32 = 0.5; }
class C_proxy { constructor(a, b) { this.k_proxy = a; this.y = b; } }
function make_proxy(v) { const o = new C_proxy(1.5, 2); const p = new Proxy(o, {}); p.k_proxy = v; return o; }
function read_proxy(o) { return o.k_proxy; }
function sum_proxy(o) { return o.k_proxy * 2 + o.y; }
function restore_proxy(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_proxy') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_proxy') && Object.getOwnPropertyDescriptor(o, 'k_proxy').writable) o.k_proxy = 0.5; }
class C_overflow { constructor(a, b) { this.k_overflow = a; this.y = b; } }
class B_overflow { constructor(a) { this.a0 = 0; this.a1 = 1; this.a2 = 2; this.a3 = 3; this.a4 = 4; this.a5 = 5; this.a6 = 6; this.a7 = 7; this.a8 = 8; this.a9 = 9; this.k_overflow = a; this.y = 2; } }
function make_overflow(v) { const o = new B_overflow(1.5); o.k_overflow = v; return o; }
function read_overflow(o) { return o.k_overflow; }
function sum_overflow(o) { return o.k_overflow * 2 + o.y; }
function restore_overflow(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_overflow') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_overflow') && Object.getOwnPropertyDescriptor(o, 'k_overflow').writable) o.k_overflow = 0.5; }
class C_poly { constructor(a, b) { this.k_poly = a; this.y = b; } }
class D_poly { constructor(a, b) { this.z = 0; this.k_poly = a; this.y = b; } }
function make_poly(v) { const o = (v === TRAIN2) ? new D_poly(1.5, 2) : new C_poly(1.5, 2); o.k_poly = v === TRAIN2 ? 3.5 : v; return o; }
function read_poly(o) { return o.k_poly; }
function sum_poly(o) { return o.k_poly * 2 + o.y; }
function restore_poly(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_poly') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_poly') && Object.getOwnPropertyDescriptor(o, 'k_poly').writable) o.k_poly = 0.5; }
class C_superSet { constructor(a, b) { this.k_superSet = a; this.y = b; } }
class E_superSet extends C_superSet { put(v) { super.k_superSet = v; } }
function make_superSet(v) { const o = new E_superSet(1.5, 2); o.put(v); return o; }
function read_superSet(o) { return o.k_superSet; }
function sum_superSet(o) { return o.k_superSet * 2 + o.y; }
function restore_superSet(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_superSet') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_superSet') && Object.getOwnPropertyDescriptor(o, 'k_superSet').writable) o.k_superSet = 0.5; }
class C_defineAll { constructor(a, b) { this.k_defineAll = a; this.y = b; } }
function make_defineAll(v) { const o = new C_defineAll(1.5, 2); Object.defineProperties(o, { k_defineAll: { value: v, writable: true, enumerable: true, configurable: true } }); return o; }
function read_defineAll(o) { return o.k_defineAll; }
function sum_defineAll(o) { return o.k_defineAll * 2 + o.y; }
function restore_defineAll(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_defineAll') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_defineAll') && Object.getOwnPropertyDescriptor(o, 'k_defineAll').writable) o.k_defineAll = 0.5; }
class C_reproto { constructor(a, b) { this.k_reproto = a; this.y = b; } }
const PROTO_reproto = { hello: 1 };
function make_reproto(v) { const o = new C_reproto(1.5, 2); Object.setPrototypeOf(o, PROTO_reproto); o.k_reproto = v; return o; }
function read_reproto(o) { return o.k_reproto; }
function sum_reproto(o) { return o.k_reproto * 2 + o.y; }
function restore_reproto(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_reproto') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_reproto') && Object.getOwnPropertyDescriptor(o, 'k_reproto').writable) o.k_reproto = 0.5; }
class C_mega { constructor(a, b) { this.k_mega = a; this.y = b; } }
let MI_mega = 0;
const MEGA_mega = [0, 1, 2, 3, 4, 5, 6, 7].map((n) => { const pre = "p" + n; return (a) => { const o = {}; o[pre] = n; o.k_mega = a; o.y = 2; return o; }; });
function make_mega(v) { const o = MEGA_mega[(MI_mega++) & 7](1.5); o.k_mega = v; return o; }
function read_mega(o) { return o.k_mega; }
function sum_mega(o) { return o.k_mega * 2 + o.y; }
function restore_mega(o) { if (!Object.isFrozen(o) && Object.getOwnPropertyDescriptor(o, 'k_mega') && 'value' in Object.getOwnPropertyDescriptor(o, 'k_mega') && Object.getOwnPropertyDescriptor(o, 'k_mega').writable) o.k_mega = 0.5; }
let acc = 0;
for (let i = 0; i < 20000; i++) {
  const v = (i & 7) + 0.5;
  { const o = make_assign(v); acc += read_assign(o) + sum_assign(o); }
  { const o = make_assignCold(TRAIN); acc += read_assignCold(o) + sum_assignCold(o); }
  { const o = make_compound(v); acc += read_compound(o) + sum_compound(o); }
  { const o = make_keyed(v); acc += read_keyed(o) + sum_keyed(o); }
  { const o = make_define(v); acc += read_define(o) + sum_define(o); }
  { const o = make_defineValue(v); acc += read_defineValue(o) + sum_defineValue(o); }
  { const o = make_getter(v); acc += read_getter(o) + sum_getter(o); }
  { const o = make_reflect(v); acc += read_reflect(o) + sum_reflect(o); }
  { const o = make_assignAll(v); acc += read_assignAll(o) + sum_assignAll(o); }
  { const o = make_spreadOver(v); acc += read_spreadOver(o) + sum_spreadOver(o); }
  { const o = make_spreadFrom(v); acc += read_spreadFrom(o) + sum_spreadFrom(o); }
  { const o = make_literal(v); acc += read_literal(o) + sum_literal(o); }
  { const o = make_literalLate(v); acc += read_literalLate(o) + sum_literalLate(o); }
  { const o = make_json(v); acc += read_json(o) + sum_json(o); }
  { const o = make_fromEntries(v); acc += read_fromEntries(o) + sum_fromEntries(o); }
  { const o = make_create(v); acc += read_create(o) + sum_create(o); }
  { const o = make_ctor(v); acc += read_ctor(o) + sum_ctor(o); }
  { const o = make_field(v); acc += read_field(o) + sum_field(o); }
  { const o = make_deleteReadd(v); acc += read_deleteReadd(o) + sum_deleteReadd(o); }
  { const o = make_freeze(v); acc += read_freeze(o) + sum_freeze(o); }
  { const o = make_destructure(v); acc += read_destructure(o) + sum_destructure(o); }
  { const o = make_destructObj(v); acc += read_destructObj(o) + sum_destructObj(o); }
  { const o = make_forOf(v); acc += read_forOf(o) + sum_forOf(o); }
  { const o = make_increment(v); acc += read_increment(o) + sum_increment(o); }
  { const o = make_int32(v); acc += read_int32(o) + sum_int32(o); }
  { const o = make_proxy(v); acc += read_proxy(o) + sum_proxy(o); }
  { const o = make_overflow(v); acc += read_overflow(o) + sum_overflow(o); }
  { const o = make_poly((i & 1) ? TRAIN2 : v); acc += read_poly(o) + sum_poly(o); }
  { const o = make_superSet(v); acc += read_superSet(o) + sum_superSet(o); }
  { const o = make_defineAll(v); acc += read_defineAll(o) + sum_defineAll(o); }
  { const o = make_reproto(v); acc += read_reproto(o) + sum_reproto(o); }
  { const o = make_mega(v); acc += read_mega(o) + sum_mega(o); }
}
console.log('trained', acc);
const obj = { toString() { return 'obj'; }, valueOf() { return 4; } };
const late = ['s', undefined, null, obj, true, -0, NaN, Infinity, 2147483648, -5];
for (const v of late) {
  { const o = make_assign(v); console.log('assign', show(v), show(read_assign(o)), show(sum_assign(o))); restore_assign(o); }
  { const o = make_assignCold(v); console.log('assignCold', show(v), show(read_assignCold(o)), show(sum_assignCold(o))); restore_assignCold(o); }
  { const o = make_compound(v); console.log('compound', show(v), show(read_compound(o)), show(sum_compound(o))); restore_compound(o); }
  { const o = make_keyed(v); console.log('keyed', show(v), show(read_keyed(o)), show(sum_keyed(o))); restore_keyed(o); }
  { const o = make_define(v); console.log('define', show(v), show(read_define(o)), show(sum_define(o))); restore_define(o); }
  { const o = make_defineValue(v); console.log('defineValue', show(v), show(read_defineValue(o)), show(sum_defineValue(o))); restore_defineValue(o); }
  { const o = make_getter(v); console.log('getter', show(v), show(read_getter(o)), show(sum_getter(o))); restore_getter(o); }
  { const o = make_reflect(v); console.log('reflect', show(v), show(read_reflect(o)), show(sum_reflect(o))); restore_reflect(o); }
  { const o = make_assignAll(v); console.log('assignAll', show(v), show(read_assignAll(o)), show(sum_assignAll(o))); restore_assignAll(o); }
  { const o = make_spreadOver(v); console.log('spreadOver', show(v), show(read_spreadOver(o)), show(sum_spreadOver(o))); restore_spreadOver(o); }
  { const o = make_spreadFrom(v); console.log('spreadFrom', show(v), show(read_spreadFrom(o)), show(sum_spreadFrom(o))); restore_spreadFrom(o); }
  { const o = make_literal(v); console.log('literal', show(v), show(read_literal(o)), show(sum_literal(o))); restore_literal(o); }
  { const o = make_literalLate(v); console.log('literalLate', show(v), show(read_literalLate(o)), show(sum_literalLate(o))); restore_literalLate(o); }
  { const o = make_json(v); console.log('json', show(v), show(read_json(o)), show(sum_json(o))); restore_json(o); }
  { const o = make_fromEntries(v); console.log('fromEntries', show(v), show(read_fromEntries(o)), show(sum_fromEntries(o))); restore_fromEntries(o); }
  { const o = make_create(v); console.log('create', show(v), show(read_create(o)), show(sum_create(o))); restore_create(o); }
  { const o = make_ctor(v); console.log('ctor', show(v), show(read_ctor(o)), show(sum_ctor(o))); restore_ctor(o); }
  { const o = make_field(v); console.log('field', show(v), show(read_field(o)), show(sum_field(o))); restore_field(o); }
  { const o = make_deleteReadd(v); console.log('deleteReadd', show(v), show(read_deleteReadd(o)), show(sum_deleteReadd(o))); restore_deleteReadd(o); }
  { const o = make_freeze(v); console.log('freeze', show(v), show(read_freeze(o)), show(sum_freeze(o))); restore_freeze(o); }
  { const o = make_destructure(v); console.log('destructure', show(v), show(read_destructure(o)), show(sum_destructure(o))); restore_destructure(o); }
  { const o = make_destructObj(v); console.log('destructObj', show(v), show(read_destructObj(o)), show(sum_destructObj(o))); restore_destructObj(o); }
  { const o = make_forOf(v); console.log('forOf', show(v), show(read_forOf(o)), show(sum_forOf(o))); restore_forOf(o); }
  { const o = make_increment(v); console.log('increment', show(v), show(read_increment(o)), show(sum_increment(o))); restore_increment(o); }
  { const o = make_int32(v); console.log('int32', show(v), show(read_int32(o)), show(sum_int32(o))); restore_int32(o); }
  { const o = make_proxy(v); console.log('proxy', show(v), show(read_proxy(o)), show(sum_proxy(o))); restore_proxy(o); }
  { const o = make_overflow(v); console.log('overflow', show(v), show(read_overflow(o)), show(sum_overflow(o))); restore_overflow(o); }
  { const o = make_poly(v); console.log('poly', show(v), show(read_poly(o)), show(sum_poly(o))); restore_poly(o); }
  { const o = make_superSet(v); console.log('superSet', show(v), show(read_superSet(o)), show(sum_superSet(o))); restore_superSet(o); }
  { const o = make_defineAll(v); console.log('defineAll', show(v), show(read_defineAll(o)), show(sum_defineAll(o))); restore_defineAll(o); }
  { const o = make_reproto(v); console.log('reproto', show(v), show(read_reproto(o)), show(sum_reproto(o))); restore_reproto(o); }
  { const o = make_mega(v); console.log('mega', show(v), show(read_mega(o)), show(sum_mega(o))); restore_mega(o); }
}
for (let i = 0; i < 2000; i++) {
  const v = (i & 3) + 0.25;
  { const o = make_assign(v); acc += read_assign(o) + sum_assign(o); }
  { const o = make_compound(v); acc += read_compound(o) + sum_compound(o); }
  { const o = make_keyed(v); acc += read_keyed(o) + sum_keyed(o); }
  { const o = make_define(v); acc += read_define(o) + sum_define(o); }
  { const o = make_defineValue(v); acc += read_defineValue(o) + sum_defineValue(o); }
  { const o = make_getter(v); acc += read_getter(o) + sum_getter(o); }
  { const o = make_reflect(v); acc += read_reflect(o) + sum_reflect(o); }
  { const o = make_assignAll(v); acc += read_assignAll(o) + sum_assignAll(o); }
  { const o = make_spreadOver(v); acc += read_spreadOver(o) + sum_spreadOver(o); }
  { const o = make_spreadFrom(v); acc += read_spreadFrom(o) + sum_spreadFrom(o); }
  { const o = make_literal(v); acc += read_literal(o) + sum_literal(o); }
  { const o = make_literalLate(v); acc += read_literalLate(o) + sum_literalLate(o); }
  { const o = make_json(v); acc += read_json(o) + sum_json(o); }
  { const o = make_fromEntries(v); acc += read_fromEntries(o) + sum_fromEntries(o); }
  { const o = make_create(v); acc += read_create(o) + sum_create(o); }
  { const o = make_ctor(v); acc += read_ctor(o) + sum_ctor(o); }
  { const o = make_field(v); acc += read_field(o) + sum_field(o); }
  { const o = make_deleteReadd(v); acc += read_deleteReadd(o) + sum_deleteReadd(o); }
  { const o = make_freeze(v); acc += read_freeze(o) + sum_freeze(o); }
  { const o = make_destructure(v); acc += read_destructure(o) + sum_destructure(o); }
  { const o = make_destructObj(v); acc += read_destructObj(o) + sum_destructObj(o); }
  { const o = make_forOf(v); acc += read_forOf(o) + sum_forOf(o); }
  { const o = make_increment(v); acc += read_increment(o) + sum_increment(o); }
  { const o = make_int32(v); acc += read_int32(o) + sum_int32(o); }
  { const o = make_proxy(v); acc += read_proxy(o) + sum_proxy(o); }
  { const o = make_overflow(v); acc += read_overflow(o) + sum_overflow(o); }
  { const o = make_superSet(v); acc += read_superSet(o) + sum_superSet(o); }
  { const o = make_defineAll(v); acc += read_defineAll(o) + sum_defineAll(o); }
  { const o = make_reproto(v); acc += read_reproto(o) + sum_reproto(o); }
  { const o = make_mega(v); acc += read_mega(o) + sum_mega(o); }
}
console.log('after', acc);
