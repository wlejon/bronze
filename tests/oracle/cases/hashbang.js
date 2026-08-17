#!/usr/bin/env bronze
// A HashbangComment on line 1 (ECMA-262 12.5), which is what makes a compiled
// script also a runnable file on a POSIX system.
//
// Derived from ECMA-262:
//
// 1. 12.5 makes `#!` at the START of the source text a comment running to the
//    end of the line. It is a production of Script and Module rather than a
//    trivia rule, which is why it is legal only at offset zero: a `#!` after
//    even one space or one blank line is still the stray-`#` error. That
//    negative is not pinnable here — this harness compares the STDOUT of a
//    program that built, and a file whose second line starts `#!` never
//    builds — so it lives as the lexer doctest `hashbang is only a comment at
//    the very start of the source`.
// 2. Only the FIRST line is a comment. `#` keeps every other meaning it has —
//    a private field is `#x` (12.7.2 PrivateIdentifier) and nothing about that
//    changes.
// 3. The line terminator after it is ordinary trivia, so the first statement
//    behaves exactly as it would in a file without the hashbang — including for
//    automatic semicolon insertion, which needs the newline to still be there.
console.log('hashbang ran');

// 2: `#` still introduces a private name.
class Counter {
  #n = 0;
  bump() {
    this.#n += 1;
    return this.#n;
  }
  static has(o) {
    return #n in o;
  }
}
const c = new Counter();
console.log(c.bump(), c.bump());
console.log(Counter.has(c), Counter.has({}));
console.log(Object.keys(c).length);

// 3: the newline after the hashbang is real trivia — ASI still works, and the
// first line of code is line 18 of the file rather than line 1 of a fragment.
const a = 1
const b = 2
console.log(a + b)
