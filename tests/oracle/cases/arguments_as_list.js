// A function that only reads `arguments.length` and `arguments[i]` is handed
// its arguments as a plain Array (ast::readsArgumentsAsList), which must read
// the same as the arguments object does; one that writes, calls through,
// passes on or otherwise mentions `arguments` still gets the object.

function emit(event, a1, a2) {
  var len = arguments.length, out = [];
  for (var i = 1; i < len; i++) out.push(arguments[i]);
  return len + ':' + out.join(',') + ':' + arguments[9];
}
function passedOn() { return Array.prototype.slice.call(arguments).length + typeof arguments.callee; }
function written() { arguments[0] = 5; return arguments[0]; }
function called() { return arguments[0](); }
const o = { m() { const f = () => arguments.length; return f(); } };

console.log(emit('x'));
console.log(emit('x', 1, 2, 3, 4));
console.log(passedOn(1, 2));
console.log(written(1));
console.log(called(function () { return this.length; }, 2, 3));
console.log(o.m(1, 2, 3));
