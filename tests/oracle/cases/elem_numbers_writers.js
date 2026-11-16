// NUMBERS arrays (runtime/array.h): the warm-up writes Numbers through every
// writer and sums the array at one read site per writer (planned on the bit);
// the calls after it write non-Numbers or leave holes, which each writer
// must answer by clearing the bit. Generated (one function per site).
function base() { const a = []; a.push(1.5, 2.5, 3.5, 4.5); return a; }
function rest(...r) { return r; }
const show = (v) => typeof v === 'number' ? String(v) : typeof v + ':' + String(v);
function sum_index(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_indexKey(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_indexLoop(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_push(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_pushMany(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_pushApply(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_unshift(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_shiftUnshift(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_splice(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_spliceSwap(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_fill(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_fillCall(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_define(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_reflect(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_assign(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_grow(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_growIndex(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_shrinkPush(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_popPush(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_appendIndex(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_reverseAfter(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_copyWithin(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_compound(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_deleteHole(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_sortHole(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_empty(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_emptyPush(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_spread(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_rest(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_destructRest(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_literal(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_concat(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_mapped(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_from(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_sliced(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function sum_filtered(a) { let s = 0; for (let i = 0; i < a.length; i++) s += a[i]; return s; }
function make_index(v) { const a = base(); a[1] = v; return a; }
function make_indexKey(v) { const a = base(); a['2'] = v; return a; }
function make_indexLoop(v) { const a = base(); for (let i = 0; i < a.length; i++) a[i] = i === 2 ? v : i; return a; }
function make_push(v) { const a = base(); a.push(v); return a; }
function make_pushMany(v) { const a = base(); a.push(0.5, v, 0.25); return a; }
function make_pushApply(v) { const a = base(); Array.prototype.push.apply(a, [v, 1]); return a; }
function make_unshift(v) { const a = base(); a.unshift(v); return a; }
function make_shiftUnshift(v) { const a = base(); a.shift(); a.unshift(v); return a; }
function make_splice(v) { const a = base(); a.splice(1, 0, v); return a; }
function make_spliceSwap(v) { const a = base(); a.splice(1, 1, v); return a; }
function make_fill(v) { const a = base(); a.fill(v, 1, 3); return a; }
function make_fillCall(v) { const a = base(); Array.prototype.fill.call(a, v, 2); return a; }
function make_define(v) { const a = base(); Object.defineProperty(a, 2, { value: v, writable: true, enumerable: true, configurable: true }); return a; }
function make_reflect(v) { const a = base(); Reflect.set(a, 1, v); return a; }
function make_assign(v) { const a = base(); Object.assign(a, { 3: v }); return a; }
function make_grow(v) { const a = base(); a.length = 6; if (typeof v === 'number') { a[4] = v; a[5] = v; } return a; }
function make_growIndex(v) { const a = base(); a[5] = v; if (typeof v === 'number') a[4] = v; return a; }
function make_shrinkPush(v) { const a = base(); a.length = 0; a.push(1.5, v); return a; }
function make_popPush(v) { const a = base(); a.pop(); a.push(v); return a; }
function make_appendIndex(v) { const a = base(); a[a.length] = v; return a; }
function make_reverseAfter(v) { const a = base(); a[0] = v; a.reverse(); return a; }
function make_copyWithin(v) { const a = base(); a[0] = v; a.copyWithin(2, 0, 1); return a; }
function make_compound(v) { const a = base(); a[2] += typeof v === 'number' ? v : 'q'; return a; }
function make_deleteHole(v) { const a = base(); delete a[1]; if (typeof v === 'number') a[1] = v; return a; }
function make_sortHole(v) { const a = base(); if (typeof v !== 'number') a[6] = 1; a.sort(); return a; }
function make_empty(v) { const a = []; a[0] = 1; a[1] = v; a[2] = 2; return a; }
function make_emptyPush(v) { const a = []; a.push(1); a.push(v); return a; }
function make_spread(v) { const a = [...base(), v]; return a; }
function make_rest(v) { const a = rest(1.5, v, 2.5); return a; }
function make_destructRest(v) { const [, ...a] = [0.5, v, 1.5, 2.5]; return a; }
function make_literal(v) { const a = [1.5, v, 2.5]; return a; }
function make_concat(v) { const a = base().concat([v]); return a; }
function make_mapped(v) { const a = base().map((x, i) => (i === 1 ? v : x)); return a; }
function make_from(v) { const a = Array.from(base(), (x, i) => (i === 2 ? v : x)); return a; }
function make_sliced(v) { const b = base(); b[1] = v; const a = b.slice(0); return a; }
function make_filtered(v) { const a = [1, v, 2, 3].filter(() => true); return a; }
let acc = 0;
for (let i = 0; i < 20000; i++) {
  const v = (i & 7) + 0.5;
  acc += sum_index(make_index(v));
  acc += sum_indexKey(make_indexKey(v));
  acc += sum_indexLoop(make_indexLoop(v));
  acc += sum_push(make_push(v));
  acc += sum_pushMany(make_pushMany(v));
  acc += sum_pushApply(make_pushApply(v));
  acc += sum_unshift(make_unshift(v));
  acc += sum_shiftUnshift(make_shiftUnshift(v));
  acc += sum_splice(make_splice(v));
  acc += sum_spliceSwap(make_spliceSwap(v));
  acc += sum_fill(make_fill(v));
  acc += sum_fillCall(make_fillCall(v));
  acc += sum_define(make_define(v));
  acc += sum_reflect(make_reflect(v));
  acc += sum_assign(make_assign(v));
  acc += sum_grow(make_grow(v));
  acc += sum_growIndex(make_growIndex(v));
  acc += sum_shrinkPush(make_shrinkPush(v));
  acc += sum_popPush(make_popPush(v));
  acc += sum_appendIndex(make_appendIndex(v));
  acc += sum_reverseAfter(make_reverseAfter(v));
  acc += sum_copyWithin(make_copyWithin(v));
  acc += sum_compound(make_compound(v));
  acc += sum_deleteHole(make_deleteHole(v));
  acc += sum_sortHole(make_sortHole(v));
  acc += sum_empty(make_empty(v));
  acc += sum_emptyPush(make_emptyPush(v));
  acc += sum_spread(make_spread(v));
  acc += sum_rest(make_rest(v));
  acc += sum_destructRest(make_destructRest(v));
  acc += sum_literal(make_literal(v));
  acc += sum_concat(make_concat(v));
  acc += sum_mapped(make_mapped(v));
  acc += sum_from(make_from(v));
  acc += sum_sliced(make_sliced(v));
  acc += sum_filtered(make_filtered(v));
}
console.log(acc);
{
  const obj = { valueOf() { return 7; } };
  console.log('index', show(sum_index(make_index('x'))), show(sum_index(make_index(undefined))), show(sum_index(make_index(obj))), show(sum_index(make_index(null))));
  console.log('indexKey', show(sum_indexKey(make_indexKey('x'))), show(sum_indexKey(make_indexKey(undefined))), show(sum_indexKey(make_indexKey(obj))), show(sum_indexKey(make_indexKey(null))));
  console.log('indexLoop', show(sum_indexLoop(make_indexLoop('x'))), show(sum_indexLoop(make_indexLoop(undefined))), show(sum_indexLoop(make_indexLoop(obj))), show(sum_indexLoop(make_indexLoop(null))));
  console.log('push', show(sum_push(make_push('x'))), show(sum_push(make_push(undefined))), show(sum_push(make_push(obj))), show(sum_push(make_push(null))));
  console.log('pushMany', show(sum_pushMany(make_pushMany('x'))), show(sum_pushMany(make_pushMany(undefined))), show(sum_pushMany(make_pushMany(obj))), show(sum_pushMany(make_pushMany(null))));
  console.log('pushApply', show(sum_pushApply(make_pushApply('x'))), show(sum_pushApply(make_pushApply(undefined))), show(sum_pushApply(make_pushApply(obj))), show(sum_pushApply(make_pushApply(null))));
  console.log('unshift', show(sum_unshift(make_unshift('x'))), show(sum_unshift(make_unshift(undefined))), show(sum_unshift(make_unshift(obj))), show(sum_unshift(make_unshift(null))));
  console.log('shiftUnshift', show(sum_shiftUnshift(make_shiftUnshift('x'))), show(sum_shiftUnshift(make_shiftUnshift(undefined))), show(sum_shiftUnshift(make_shiftUnshift(obj))), show(sum_shiftUnshift(make_shiftUnshift(null))));
  console.log('splice', show(sum_splice(make_splice('x'))), show(sum_splice(make_splice(undefined))), show(sum_splice(make_splice(obj))), show(sum_splice(make_splice(null))));
  console.log('spliceSwap', show(sum_spliceSwap(make_spliceSwap('x'))), show(sum_spliceSwap(make_spliceSwap(undefined))), show(sum_spliceSwap(make_spliceSwap(obj))), show(sum_spliceSwap(make_spliceSwap(null))));
  console.log('fill', show(sum_fill(make_fill('x'))), show(sum_fill(make_fill(undefined))), show(sum_fill(make_fill(obj))), show(sum_fill(make_fill(null))));
  console.log('fillCall', show(sum_fillCall(make_fillCall('x'))), show(sum_fillCall(make_fillCall(undefined))), show(sum_fillCall(make_fillCall(obj))), show(sum_fillCall(make_fillCall(null))));
  console.log('define', show(sum_define(make_define('x'))), show(sum_define(make_define(undefined))), show(sum_define(make_define(obj))), show(sum_define(make_define(null))));
  console.log('reflect', show(sum_reflect(make_reflect('x'))), show(sum_reflect(make_reflect(undefined))), show(sum_reflect(make_reflect(obj))), show(sum_reflect(make_reflect(null))));
  console.log('assign', show(sum_assign(make_assign('x'))), show(sum_assign(make_assign(undefined))), show(sum_assign(make_assign(obj))), show(sum_assign(make_assign(null))));
  console.log('grow', show(sum_grow(make_grow('x'))), show(sum_grow(make_grow(undefined))), show(sum_grow(make_grow(obj))), show(sum_grow(make_grow(null))));
  console.log('growIndex', show(sum_growIndex(make_growIndex('x'))), show(sum_growIndex(make_growIndex(undefined))), show(sum_growIndex(make_growIndex(obj))), show(sum_growIndex(make_growIndex(null))));
  console.log('shrinkPush', show(sum_shrinkPush(make_shrinkPush('x'))), show(sum_shrinkPush(make_shrinkPush(undefined))), show(sum_shrinkPush(make_shrinkPush(obj))), show(sum_shrinkPush(make_shrinkPush(null))));
  console.log('popPush', show(sum_popPush(make_popPush('x'))), show(sum_popPush(make_popPush(undefined))), show(sum_popPush(make_popPush(obj))), show(sum_popPush(make_popPush(null))));
  console.log('appendIndex', show(sum_appendIndex(make_appendIndex('x'))), show(sum_appendIndex(make_appendIndex(undefined))), show(sum_appendIndex(make_appendIndex(obj))), show(sum_appendIndex(make_appendIndex(null))));
  console.log('reverseAfter', show(sum_reverseAfter(make_reverseAfter('x'))), show(sum_reverseAfter(make_reverseAfter(undefined))), show(sum_reverseAfter(make_reverseAfter(obj))), show(sum_reverseAfter(make_reverseAfter(null))));
  console.log('copyWithin', show(sum_copyWithin(make_copyWithin('x'))), show(sum_copyWithin(make_copyWithin(undefined))), show(sum_copyWithin(make_copyWithin(obj))), show(sum_copyWithin(make_copyWithin(null))));
  console.log('compound', show(sum_compound(make_compound('x'))), show(sum_compound(make_compound(undefined))), show(sum_compound(make_compound(obj))), show(sum_compound(make_compound(null))));
  console.log('deleteHole', show(sum_deleteHole(make_deleteHole('x'))), show(sum_deleteHole(make_deleteHole(undefined))), show(sum_deleteHole(make_deleteHole(obj))), show(sum_deleteHole(make_deleteHole(null))));
  console.log('sortHole', show(sum_sortHole(make_sortHole('x'))), show(sum_sortHole(make_sortHole(undefined))), show(sum_sortHole(make_sortHole(obj))), show(sum_sortHole(make_sortHole(null))));
  console.log('empty', show(sum_empty(make_empty('x'))), show(sum_empty(make_empty(undefined))), show(sum_empty(make_empty(obj))), show(sum_empty(make_empty(null))));
  console.log('emptyPush', show(sum_emptyPush(make_emptyPush('x'))), show(sum_emptyPush(make_emptyPush(undefined))), show(sum_emptyPush(make_emptyPush(obj))), show(sum_emptyPush(make_emptyPush(null))));
  console.log('spread', show(sum_spread(make_spread('x'))), show(sum_spread(make_spread(undefined))), show(sum_spread(make_spread(obj))), show(sum_spread(make_spread(null))));
  console.log('rest', show(sum_rest(make_rest('x'))), show(sum_rest(make_rest(undefined))), show(sum_rest(make_rest(obj))), show(sum_rest(make_rest(null))));
  console.log('destructRest', show(sum_destructRest(make_destructRest('x'))), show(sum_destructRest(make_destructRest(undefined))), show(sum_destructRest(make_destructRest(obj))), show(sum_destructRest(make_destructRest(null))));
  console.log('literal', show(sum_literal(make_literal('x'))), show(sum_literal(make_literal(undefined))), show(sum_literal(make_literal(obj))), show(sum_literal(make_literal(null))));
  console.log('concat', show(sum_concat(make_concat('x'))), show(sum_concat(make_concat(undefined))), show(sum_concat(make_concat(obj))), show(sum_concat(make_concat(null))));
  console.log('mapped', show(sum_mapped(make_mapped('x'))), show(sum_mapped(make_mapped(undefined))), show(sum_mapped(make_mapped(obj))), show(sum_mapped(make_mapped(null))));
  console.log('from', show(sum_from(make_from('x'))), show(sum_from(make_from(undefined))), show(sum_from(make_from(obj))), show(sum_from(make_from(null))));
  console.log('sliced', show(sum_sliced(make_sliced('x'))), show(sum_sliced(make_sliced(undefined))), show(sum_sliced(make_sliced(obj))), show(sum_sliced(make_sliced(null))));
  console.log('filtered', show(sum_filtered(make_filtered('x'))), show(sum_filtered(make_filtered(undefined))), show(sum_filtered(make_filtered(obj))), show(sum_filtered(make_filtered(null))));
}
