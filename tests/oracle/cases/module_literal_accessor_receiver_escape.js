// A module-scope object literal that hands ITSELF out through a method call.
//
// `X.m(...)` makes `this` be `X` inside `m` (ECMA-262 13.3.6.2: the call's
// this-value is the base of the member reference), so a literal whose binding
// is never written down as a value can still end up in someone else's hands —
// and once it is there, `Object.defineProperty` can replace the accessor that
// the reads of `X.p` all around the program go through.
//
// Three ways in, one answer each time: the read after the redefinition is the
// redefinition.

const registry = [];

function keep(tag) { registry.push(this); return tag; }

// (1) The member's function is written somewhere else entirely, so reading the
// literal says nothing about what `this` meets.
const Named = {
  _q: 'named-original',
  get q() { return this._q; },
  hold: keep
};
console.log('named before  ' + Named.q);
console.log('named hold    ' + Named.hold('kept-1'));
Object.defineProperty(registry[0], 'q', {
  get() { return 'named-through-alias'; },
  configurable: true
});
console.log('named after   ' + Named.q);

// (2) The member IS written inline, and hands `this` on itself.
const Inline = {
  _q: 'inline-original',
  get q() { return this._q; },
  hold(tag) { registry.push(this); return tag; }
};
console.log('inline before ' + Inline.q);
console.log('inline hold   ' + Inline.hold('kept-2'));
Object.defineProperty(registry[1], 'q', {
  get() { return 'inline-through-alias'; },
  configurable: true
});
console.log('inline after  ' + Inline.q);

// (3) The member written inline is harmless; the one that runs is not, because
// a write replaced it between the two calls.
const Swapped = {
  _q: 'swapped-original',
  get q() { return this._q; },
  run(tag) { return 'own-' + tag; }
};
console.log('swap own      ' + Swapped.run('a'));
Swapped.run = function (tag) { registry.push(this); return 'new-' + tag; };
console.log('swap new      ' + Swapped.run('b'));
Object.defineProperty(registry[2], 'q', {
  get() { return 'swapped-through-alias'; },
  configurable: true
});
console.log('swap after    ' + Swapped.q);
