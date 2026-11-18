// A `var` is bound when its function (or script) is entered, wherever the
// declaration is written, and `var x;` with no initializer evaluates to
// nothing (14.3.2.1). A write that runs before the declaration therefore keeps
// its value past it. d3's formatSpecifier is
// `if (!(match = re.exec(s))) throw ...; var match;`, and every d3 axis read
// `match` back as undefined.

function d3Shape(s) { if (!(m = /^(a)(b)$/.exec(s))) throw new Error('no match'); var m; return m[2]; }
function declaredLast(s) { if (!(m = /^(a)(b)$/.exec(s))) throw new Error('x'); return m[2]; var m; }
function plain(s) { m = /^(a)(b)$/.exec(s); var m; return m[2]; }
function literal() { if (!(m = [1, 'b', 'b'])) throw new Error('x'); var m; return m[2]; }
function inBlock() { x = 1; { var x; } return x; }
function inLoop() { for (let k = 0; k < 2; k++) { y = k; } if (false) { var y; } return y; }
function inTrySwitch() { try { z = 5; } catch (e) {} switch (1) { case 1: var z; } return z; }
function captured() { const r = () => w; w = 7; var w; return r(); }
function parameter(q) { q = 3; var q; return q; }
function laterFor() { i = 9; for (var i = 0; i < 2; i++); return i; }
function update() { n++; var n = 4; return n; }
function compound() { v += 'a'; var v; return v; }
function loopRedeclares() {
  const seen = [];
  for (let k = 0; k < 3; k++) { var acc; seen.push(acc); acc = k; }
  return seen.join(',');
}
function catchParam() { try { throw 1; } catch (e) { var e; return e; } }

console.log(d3Shape('ab'), declaredLast('ab'), plain('ab'), literal());
console.log(inBlock(), inLoop(), inTrySwitch(), captured(), parameter(1), laterFor());
console.log(update(), compound(), loopRedeclares(), catchParam());

// None of those writes reached the global object.
console.log(['m', 'x', 'y', 'z', 'w', 'q', 'i', 'n', 'v', 'acc'].filter((k) => k in globalThis).join(',') || 'none');
