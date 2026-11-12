// `f.apply(t, arguments)`: a function that only forwards its arguments.
const log = [];
const target = {
    name: 'target',
    take() { log.push(this.name + ':' + arguments.length + ':' + Array.prototype.join.call(arguments, '|')); return arguments.length; },
    sum(a, b, c) { return (a || 0) + (b || 0) + (c || 0); },
};
function forward() { return target.take.apply(target, arguments); }
function forwardSum() { try { return target.sum.apply(null, arguments); } catch (e) { return 'threw'; } }
console.log(forward(), forward(1), forward(1, 'two', 3.5, null, undefined, { a: 1 }.a, [7, 8]));
console.log(forward(1, 2, 3, 4, 5, 6, 7, 8, 9), forwardSum(), forwardSum(1), forwardSum(1, 2, 3, 4));
console.log(log.join(' '));

// The forwarder also reading the length and an element.
function counted() { return arguments.length + ':' + arguments[0] + ':' + target.sum.apply(target, arguments); }
console.log(counted(), counted(4), counted(4, 5, 6));

// In a loop, with a receiver from the arguments' own function.
function Emitter() { this.seen = []; }
Emitter.prototype.record = function () { this.seen.push(arguments.length); return this; };
Emitter.prototype.emit = function () { return this.record.apply(this, arguments); };
const em = new Emitter();
for (let i = 0; i < 2000; i++) em.emit(i, i + 1).emit().emit(i);
console.log(em.seen.length, em.seen.slice(0, 6).join());

// An arrow reads the enclosing function's arguments.
function viaArrow() { const go = () => target.sum.apply(null, arguments); return go(); }
console.log(viaArrow(1, 2), viaArrow(10, 20, 30, 40));

// The list is a copy taken at the call: the callee's own arguments are its own.
function mutate() { arguments[0] = 'changed'; return arguments[0] + ',' + arguments.length; }
function outer(a) { const r = mutate.apply(null, arguments); return r + ',' + a + ',' + arguments[0]; }
console.log(outer('kept', 2));

// Other receivers of an `apply` call, and other lists.
console.log(Math.max.apply(null, [3, 9, 4]), Math.max.apply(Math, []), Math.max.apply(null), Math.max.apply(null, null));
console.log(String.fromCharCode.apply(null, [104, 105]), target.sum.apply(null, { length: 2, 0: 5, 1: 6 }));
const sparse = [1, , 3];
console.log(target.take.apply(target, sparse), log[log.length - 1]);
function maxOf() { return Math.max.apply(Math, arguments); }
console.log(maxOf(1, 5, 2), maxOf(), maxOf(-1));
try { (function () { return (void 0).apply(null, arguments); })(1); } catch (e) { console.log(e instanceof TypeError); }
try { (function () { return ({}).apply(null, arguments); })(1); } catch (e) { console.log(e instanceof TypeError); }
const notFn = { apply(t, list) { return Array.isArray(list) ? 'array' : typeof list; } };
console.log(typeof (function () { return notFn.apply(null, arguments); })(1, 2));

// A function that also hands `arguments` elsewhere keeps the full object.
function full() { return Object.prototype.toString.call(arguments) + target.sum.apply(null, arguments); }
console.log(typeof full(1, 2));
