// A truth test of a value the loop does not change is computed once, ahead
// of the loop (src/lower/loop_truth_hoist.h). ToBoolean runs no code, so the
// only thing that may show is the answer: every kind of value, a loop that
// never runs, a value the loop does change, and a test inside a try.

function keep(list, fn, context, once) {
  var out = [];
  for (var i = 0; i < list.length; i++) {
    if (list[i].fn !== fn || (once && !list[i].once) || (context && list[i].context !== context)) {
      out.push(list[i].id);
    }
  }
  return out.join(',');
}
var f = function () {}, g = function () {}, ctx = {}, other = {};
var list = [
  { id: 1, fn: f, context: ctx, once: false },
  { id: 2, fn: g, context: ctx, once: true },
  { id: 3, fn: f, context: other, once: true },
  { id: 4, fn: f, context: ctx, once: true },
];
console.log(keep(list, f), '|', keep(list, f, ctx), '|', keep(list, f, undefined, true), '|', keep(list, f, other, true));
console.log(keep([], f, ctx, true) === '', keep(list, g, null, 0), '|', keep(list, g, '', NaN), '|', keep(list, g, 'x', 1n));

// Every kind of value, tested on each turn.
function count(n, v) {
  var c = 0;
  for (var i = 0; i < n; i++) if (v) c++; else c--;
  return c;
}
var values = [undefined, null, true, false, 0, -0, NaN, 1, -1.5, Infinity, '', '0', 'a', 0n, 7n, {}, [], f, Symbol('s'),
              new Boolean(false), new Number(0), new String('')];
console.log(values.map(function (v) { return count(3, v); }).join(' '));
console.log(count(0, true), count(0, false));

// The loop changes the value: tested on each turn as before.
function flip(n, v) {
  var s = '';
  for (var i = 0; i < n; i++) {
    s += v ? 'T' : 'F';
    v = !v;
  }
  return s;
}
console.log(flip(5, 1), flip(4, ''));
function drain(v) {
  var n = 0;
  while (v) { v = v.next; n++; }
  return n;
}
console.log(drain({ next: { next: { next: null } } }), drain(0));

// Nested loops, a test in the inner one of a value from outside both, and of
// one the outer loop sets.
function grid(rows, mark) {
  var n = 0;
  for (var r = 0; r < rows.length; r++) {
    var row = rows[r];
    for (var c = 0; c < 3; c++) {
      if (mark) n += 10;
      if (row) n += row[c];
    }
  }
  return n;
}
console.log(grid([[1, 2, 3], null, [4, 5, 6], 0], true), grid([[1, 2, 3], undefined], 0), grid([], {}));

// Inside a try, with a throw in the loop, and a closure variable.
function guarded(list, strict) {
  var seen = [];
  for (var i = 0; i < list.length; i++) {
    try {
      if (strict && list[i] < 0) throw new RangeError('neg ' + list[i]);
      seen.push(strict ? list[i] * 2 : list[i]);
    } catch (e) {
      seen.push(e.message);
    }
  }
  return seen.join(',');
}
console.log(guarded([1, -2, 3], true), '|', guarded([1, -2, 3], false), '|', guarded([1, -2, 3], 'yes'));
function captured(n) {
  var on = false, log = '';
  var toggle = function () { on = !on; };
  for (var i = 0; i < n; i++) {
    if (on) log += i;
    if (i % 2 === 0) toggle();
  }
  return log;
}
console.log(captured(7));
