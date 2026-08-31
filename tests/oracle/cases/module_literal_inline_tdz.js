// Calling a method of a module-scope object literal before the literal exists.
//
// A `const` at module scope is in a temporal dead zone from the top of the
// module until its own declaration initialises it (ECMA-262 14.3.1). A hoisted
// `function` declaration and an already-evaluated `class` are initialised
// before any of that runs, so a call made from inside one, above the `const`,
// reads a binding that is still in its dead zone — and 9.1.1.1.6
// GetBindingValue throws a ReferenceError rather than answering.
//
// The two call positions are here for different reasons. A function called
// above the declaration is the direct case. A CLASS METHOD is the case that
// cannot be settled by looking at where it was written: the method body sits
// below nothing in particular, and when it runs is decided by where it is
// invoked from — so the check it carries has to be the real one.

class Probe {
  run(n) { return Space.probe(n); }
}

function early() { return Space.probe(1); }

try {
  console.log('early fn      ' + early());
} catch (e) {
  console.log('early fn      ' + e.name);
}

const probe = new Probe();

try {
  console.log('early method  ' + probe.run(1));
} catch (e) {
  console.log('early method  ' + e.name);
}

const Space = {
  base: 5,
  probe: function (n) {
    if (n === 1) return this.base;
    return 'other';
  }
};

console.log('late fn       ' + early());
console.log('late method   ' + probe.run(1));
console.log('late other    ' + probe.run(2));

// The same read a thousand times from the class method, now that the binding
// holds a value: whatever a compiler does with the dead-zone check, it has to
// keep answering the same way once the zone is over.
let hits = 0;
for (let i = 0; i < 1000; i++) {
  if (probe.run(1) === 5) hits++;
}
console.log('hot           ' + hits);
