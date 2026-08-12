// The edges of the temporal dead zone: which declarations have one, which do
// not, and what a read, a write and a `typeof` each do inside it. The three
// straightforward shapes are in temporal_dead_zone.js; these are the ones that
// decide whether the mechanism is the language's or merely close to it.
//
//   - a hoisted `function` declaration has NO dead zone. 8.6.2 instantiates it
//     for the whole scope before any statement runs, so calling it above where
//     it is written is ordinary JavaScript and must never become an error;
//   - `typeof` does not spare it. 13.5.3 step 1 excuses only an UNRESOLVABLE
//     reference; a `let` in its dead zone is perfectly resolvable, so GetValue
//     runs and throws;
//   - a `class` declaration is lexical too (15.7), dead zone and all — and
//     `class C extends C` is inside its own, because 15.7.14 evaluates the
//     heritage at step 5 and initializes the class binding only at step 17;
//   - a WRITE throws as well: 6.2.5.6 PutValue reaches 9.1.1.1.5
//     SetMutableBinding, which refuses an uninitialized binding exactly as
//     GetBindingValue does. Answering the write instead would let a program
//     initialize a binding out of its own dead zone;
//   - `let seed = seed` reads the binding its own declaration is creating,
//     because 14.3.1.2 creates the binding and then evaluates the initializer;
//   - a loop body is a scope per ITERATION, so the dead zone is re-entered
//     every time round rather than ending once;
//   - a `var` has no dead zone at all: 8.6.2 initializes it to `undefined`
//     when the function is entered, which is why reading one early is
//     `undefined` and not an error.
{
  console.log(hoisted());
  function hoisted() { return "hoisted-ok"; }
}

{
  try {
    console.log(typeof zone);
  } catch (e) {
    console.log("typeof:" + e.name);
  }
  let zone = 1;
  console.log(typeof zone);
}

{
  try {
    new Later();
  } catch (e) {
    console.log("class:" + e.name);
  }
  class Later {
    constructor() {
      this.k = 1;
    }
  }
  console.log(new Later().k);
}

{
  try {
    class SelfExtends extends SelfExtends {}
  } catch (e) {
    console.log("extends:" + e.name);
  }
  class Base {
    tag() {
      return "base";
    }
  }
  class Derived extends Base {}
  console.log(new Derived().tag());
}

{
  let ready = "outer";
  {
    try {
      ready = "too early";
    } catch (e) {
      console.log("assign:" + e.name);
    }
    let ready = "inner";
    console.log(ready);
  }
  console.log(ready);
}

{
  let seed = "seed";
  {
    try {
      let seed = seed;
    } catch (e) {
      console.log("self:" + e.name);
    }
  }
  console.log(seed);
}

for (let i = 0; i < 2; i++) {
  try {
    console.log(inner);
  } catch (e) {
    console.log("iter" + i + ":" + e.name);
  }
  let inner = i * 10;
  console.log(inner);
}

function varIsNotLexical() {
  const before = typeof v;
  var v = 1;
  return before + ":" + v;
}
console.log(varIsNotLexical());

// A closure made ABOVE the declaration and called BELOW it. The dead zone is a
// property of the moment the read happens, so the same function value answers
// `ReferenceError` and then a number without anything about it changing.
{
  const readers = [];
  for (const n of [0, 1, 2]) {
    readers.push(function () {
      return n * 100 + offset;
    });
  }
  try {
    console.log(readers[0]());
  } catch (e) {
    console.log("closure:" + e.name);
  }
  let offset = 7;
  let out = "";
  for (const r of readers) out += r() + " ";
  console.log(out.trim());
}

// Every raised ReferenceError is a fresh heap object, and the environment
// record holding the marker is another one per iteration — so a dead zone hit
// in a loop allocates twice per turn while the array below stays live. Here to
// be run under BRONZE_GC_STRESS=1 by oracle-gc-stress.
{
  const kept = [];
  for (const n of [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]) {
    try {
      kept.push({ tag: "t" + n, value: dead });
    } catch (e) {
      kept.push({ tag: "t" + n, value: e.name });
    }
    let dead = n;
  }
  console.log(kept.length, kept[0].tag, kept[0].value, kept[9].tag, kept[9].value);
}
