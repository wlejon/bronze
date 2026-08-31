// Calling a method of a module-scope object literal.
//
// `X.m(...)` on such a literal has a receiver and a callee that are both
// compile-time facts, so a compiler may run the body where the call is instead
// of calling it. This case pins what that call answers, and pins every part of
// it a moved body could get wrong: the value `this` has inside, the order and
// the count of the argument evaluations, a guard that reads a property the
// program later writes, a slow path that must still run the real method, a
// method that reifies its own argument list, a member replaced through `this`,
// and a literal that escapes and is redefined through the alias.

const order = [];
function note(tag, value) { order.push(tag); return value; }

// --- a method that hands `this` back ---------------------------------------
// 13.3.6.1 EvaluateCall passes the base of the member expression as the this
// value, so a body that returns `this` returns the literal's own object and
// `===` on the binding sees the same one. Nothing is claimed about this
// literal: returning `this` hands the object to code that could redefine it,
// and a compiler that proves anything here has to refuse for exactly that.
const Self = {
  tag: 'self',
  me: function () { return this; },
  named: function () { return this.tag; }
};
console.log('self identity ' + (Self.me() === Self));
console.log('self tag      ' + Self.me().tag);
console.log('self named    ' + Self.named());

// --- a guarded early return behind a forwarding method ---------------------
// Two calls deep, and on most of the paths below both bodies do nothing but
// compare and return. `convert`'s tail is a real computation, so a call that
// gets past the guard has to reach it.
const Space = {
  enabled: true,
  _working: 'linear',

  convert: function (v, from, to) {
    if (this.enabled === false || from === to || !from || !to) {
      return v;
    }
    return v + '|' + from + '>' + to;
  },

  toWorking: function (v, from) {
    return this.convert(v, from, this._working);
  },

  fromWorking: function (v, to) {
    return this.convert(v, this._working, to);
  }
};

console.log('same space    ' + Space.toWorking('c', 'linear'));
console.log('to working    ' + Space.toWorking('c', 'srgb'));
console.log('from working  ' + Space.fromWorking('c', 'srgb'));
console.log('empty target  ' + Space.convert('c', 'a', ''));
console.log('empty source  ' + Space.convert('c', '', 'b'));
// 10.2.11 binds every parameter the call did not supply to `undefined`, and
// `!undefined` is true, so the guard answers on the argument that is not there.
console.log('missing arg   ' + Space.toWorking('c'));

// The guard reads a property, and a property is whatever was last written to
// it. A guard answered from the value the literal was built with would print
// the conversion here.
Space.enabled = false;
console.log('disabled      ' + Space.convert('c', 'a', 'b'));
Space.enabled = true;
console.log('re-enabled    ' + Space.convert('c', 'a', 'b'));

// 13.3.6.1 evaluates the arguments left to right, each exactly once, before
// the body runs — including the ones past the last parameter, which are
// evaluated and dropped.
order.length = 0;
console.log('order fast    ' + Space.toWorking(note('v', 'c'), note('f', 'linear')) +
            ' [' + order.join(',') + ']');
order.length = 0;
console.log('order slow    ' + Space.toWorking(note('v', 'c'), note('f', 'srgb')) +
            ' [' + order.join(',') + ']');
order.length = 0;
console.log('order extra   ' + Space.toWorking(note('v', 'c'), note('f', 'linear'),
                                               note('x', 'drop')) +
            ' [' + order.join(',') + ']');

// The same call a thousand times over, on each of the two paths: an answer
// that drifts is a cache, or a guard, that outlived what it stood for.
let hot = 0;
for (let i = 0; i < 1000; i++) {
  if (Space.toWorking('c', 'linear') === 'c') hot++;
}
console.log('hot fast      ' + hot);
let cool = 0;
for (let i = 0; i < 1000; i++) {
  if (Space.fromWorking('c', 'srgb') === 'c|linear>srgb') cool++;
}
console.log('hot slow      ' + cool);

// --- a method that reads its own argument list -----------------------------
// `arguments` is 10.2.11's mapped object over the frame the call made. The
// guard is answered before any frame exists, so the value it answers with has
// to be right; every other call has to reach a frame whose `arguments` holds
// exactly the arguments written at the site, however many there are.
const Counted = {
  base: 10,
  sum: function (a) {
    if (a === 0) return this.base;
    let total = 0;
    for (let i = 0; i < arguments.length; i++) total += arguments[i];
    return total + this.base;
  }
};
console.log('args guard    ' + Counted.sum(0));
console.log('args slow     ' + Counted.sum(1, 2, 3));
console.log('args one      ' + Counted.sum(4));

// --- a chain of guards, and the return after them --------------------------
const Chain = {
  mode: 'b',
  pick: function (k) {
    if (k === 'x') return 'first';
    if (k === this.mode) return 'second';
    if (!k) return 'third';
    return 'fallthrough';
  }
};
console.log('chain x       ' + Chain.pick('x'));
console.log('chain mode    ' + Chain.pick('b'));
console.log('chain empty   ' + Chain.pick(''));
console.log('chain other   ' + Chain.pick('z'));
console.log('chain none    ' + Chain.pick());
Chain.mode = 'z';
console.log('chain moved   ' + Chain.pick('z'));
console.log('chain b now   ' + Chain.pick('b'));

// --- typeof in a guard -----------------------------------------------------
// 13.5.3 answers from the value's type and evaluates nothing else, including
// for `null`, whose answer is "object".
const Typed = {
  fallback: 'none',
  named: function (v) {
    if (typeof v === 'string') return v;
    if (typeof v === 'undefined') return this.fallback;
    return 'other';
  }
};
console.log('typed string  ' + Typed.named('s'));
console.log('typed undef   ' + Typed.named());
console.log('typed number  ' + Typed.named(7));
console.log('typed null    ' + Typed.named(null));

// --- `&&` and `||` produce an operand, not a boolean -----------------------
const Logic = {
  on: true,
  pick: function (a, b) {
    if (this.on && a) return a;
    if (b || this.on) return 'b-or-on';
    return 'none';
  }
};
console.log('logic a       ' + Logic.pick('A', 'B'));
console.log('logic b       ' + Logic.pick('', 'B'));
Logic.on = false;
console.log('logic off     ' + Logic.pick('', ''));
console.log('logic off b   ' + Logic.pick('A', 'B'));

// --- what a body with no value returns -------------------------------------
// A bare `return;` and falling off the end are both `undefined` (10.2.1.3).
const Quiet = {
  step: 1,
  ping: function (x) { if (x === this.step) return; }
};
console.log('quiet return  ' + Quiet.ping(1));
console.log('quiet fall    ' + Quiet.ping(2));

// --- a member replaced through `this` --------------------------------------
// `arm` writes over a sibling, and `ask` calls it. Whatever is claimed about
// the members the program calls on the BINDING, `hidden` is not one of them:
// it is reached only through `this`, and it is not the function the literal
// wrote there after `arm` has run.
const Half = {
  k: 3,
  arm: function () {
    this.hidden = function () { return 'installed'; };
    return 'armed';
  },
  hidden: function () {
    if (this.k === 3) return 'original';
    return 'other';
  },
  ask: function () { return this.hidden(); }
};
console.log('half before   ' + Half.ask());
console.log('half arm      ' + Half.arm());
console.log('half after    ' + Half.ask());

// --- an escaping binding, redefined through the alias ----------------------
// `alias` and `Escapes` are the same object, so a write through one replaces
// the method the other calls.
const Escapes = {
  n: 1,
  read: function () {
    if (this.n === 1) return 'one';
    return 'other';
  }
};
const alias = Escapes;
console.log('escape before ' + Escapes.read());
alias.read = function () { return 'replaced'; };
console.log('escape after  ' + Escapes.read());

// A data property turned into an ACCESSOR through the alias. 10.1.6.3 permits
// it because a literal's property is configurable, and the guard that reads it
// then runs a getter — so the read count is pinned and not just the answer.
const Watched = {
  flag: true,
  ask: function () {
    if (this.flag === true) return 'yes';
    return 'no';
  }
};
const watchedAlias = Watched;
let reads = 0;
console.log('watched yes   ' + Watched.ask());
Object.defineProperty(watchedAlias, 'flag', {
  get() { reads++; return false; },
  configurable: true
});
console.log('watched no    ' + Watched.ask() + ' reads=' + reads);

// --- bodies that reach themselves ------------------------------------------
// Neither ring is ever entered past its guard, so nothing here recurses at run
// time. What is pinned is that the program compiles at all: a body that is
// moved into its own caller and reaches itself has to stop.
const Ring = {
  a: function (n) { if (n === 0) return 'a'; return this.b(n); },
  b: function (n) { if (n === 0) return 'b'; return this.a(n); }
};
console.log('ring a        ' + Ring.a(0));
console.log('ring b        ' + Ring.b(0));

const Selfy = {
  stop: 0,
  step: function (n) {
    if (n === this.stop) return 'stop';
    return this.step(n);
  }
};
console.log('recursive     ' + Selfy.step(0));
