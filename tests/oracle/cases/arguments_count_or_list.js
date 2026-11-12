// A function whose own body only reads `arguments.length` and `arguments[i]`
// (no arrow reads it, no rest parameter) is handed the argument COUNT when
// the caller passes no more than it has parameters, and the list otherwise
// (il.h argumentsIsCount). Either way the reads answer what the arguments
// object would: the length is the count passed, an element is the value
// passed (not what the parameter holds now, and not its default), and a read
// past the end is undefined.

function emit(event, a1, a2) {
  var len = arguments.length, out = [];
  for (var i = 1; i < len; i++) out.push(arguments[i]);
  return len + ':' + out.join(',') + ':' + arguments[9] + ':' + arguments[2];
}
console.log(emit());
console.log(emit('x'));
console.log(emit('x', 1));
console.log(emit('x', 1, 2));
console.log(emit('x', 1, 2, 3));
console.log(emit('x', undefined, null, 3, 4, 5));
console.log(emit.call(null, 'x', 7), emit.apply(null, ['x', 7, 8, 9]), emit(...[1, 2]), emit(...[1, 2, 3, 4]));

// The values the parameters arrived with, whatever they hold by the read.
function reassigned(a, b) {
  'use strict';
  a = 'changed';
  b = b + 1;
  return [arguments.length, arguments[0], arguments[1], a, b].join('|');
}
console.log(reassigned(1, 2), reassigned(1), reassigned(), reassigned(1, 2, 3));

// A default is not what was passed, and a destructured parameter is its object.
function defaults(a = 5, { k } = { k: 'd' }, c) {
  return [arguments.length, String(arguments[0]), JSON.stringify(arguments[1]), String(arguments[2]), a, k].join('|');
}
console.log(defaults(), defaults(undefined), defaults(1, { k: 2 }), defaults(1, { k: 2 }, 3, 4));

// Keys that are not plain indices, and the optional forms.
function keys(a, b) {
  var i = 1;
  return [arguments['0'], arguments[i], arguments[i + 0.5], arguments[-1], arguments['length'], arguments?.length,
          arguments?.[0], typeof arguments[1], arguments.length > 1 ? arguments[arguments.length - 1] : 'none'].join('|');
}
console.log(keys('p', 'q'), keys('p'), keys('p', 'q', 'r'));

// No parameters at all; a method; a constructor; a closure in a loop.
function none() { return arguments.length + ':' + arguments[0]; }
console.log(none(), none(1), none(1, 2));
const obj = { m(x) { return this.tag + arguments.length + arguments[0]; }, tag: 't' };
console.log(obj.m(), obj.m('a'), obj.m('a', 'b'));
function Ctor(a) { this.n = arguments.length; this.first = arguments[0]; }
console.log(JSON.stringify(new Ctor()), JSON.stringify(new Ctor(4)), JSON.stringify(new Ctor(4, 5)));
const fns = [];
for (let n = 0; n < 3; n++) fns.push(function (a) { return n + ':' + arguments.length + ':' + arguments[n]; });
console.log(fns.map((f) => f('u', 'v', 'w')).join(' '), fns.map((f) => f('u')).join(' '));

// An index with an effect runs once, in order.
function effects(a, b) {
  var log = [], i = 0;
  var v = arguments[(log.push('i'), i++)] + arguments[(log.push('j'), i++)];
  return v + ':' + log.join('') + ':' + i;
}
console.log(effects('a', 'b'), effects('a', 'b', 'c'));

// Still the list or the object: an arrow that reads it, a rest parameter, a
// nested function with its own, a shadowing declaration, and the forms that
// are not reads.
function viaArrow(a, b) { const f = () => arguments.length + ':' + arguments[1]; return f(); }
console.log(viaArrow(1), viaArrow(1, 2), viaArrow(1, 2, 3));
function withRest(a, ...r) { return arguments.length + ':' + arguments[1] + ':' + r.length; }
console.log(withRest(), withRest(1), withRest(1, 2, 3));
function nested(a) {
  function inner(x, y) { return arguments.length + ':' + arguments[0]; }
  return arguments.length + '/' + inner(a, a, a) + '/' + inner();
}
console.log(nested('n'), nested('n', 'o'));
function shadowed(a, b) {
  var n = arguments.length;
  { let arguments = { length: 'L', 0: 'Z' }; n += ':' + arguments.length + arguments[0]; }
  try { throw { length: 'C' }; } catch (arguments) { n += ':' + arguments.length; }
  return n + ':' + arguments[0];
}
console.log(shadowed('s'), shadowed('s', 't', 'u'));
function whole(a) { return typeof arguments + ':' + Array.prototype.join.call(arguments, '+'); }
console.log(whole(1), whole(1, 2));
function* gen(a) { yield arguments.length; yield arguments[0]; }
console.log([...gen('g')].join(','), [...gen('g', 'h')].join(','));
async function asy(a) { await null; return arguments.length + ':' + arguments[0]; }
asy('z').then((v) => console.log(v));
asy('z', 'y').then((v) => console.log(v));
