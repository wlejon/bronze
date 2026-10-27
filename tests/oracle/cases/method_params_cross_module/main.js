// A method inherited across a module boundary is reached by calls on the
// subclass. Every caller below passes a number on a Base receiver; the calls on
// a Sub pass a string, an object and undefined, which the inherited method has
// to receive as they are (string concatenation, not number addition).
import { Base } from './base.js';
import { Sub, Loud } from './sub.js';

const b = new Base();
const s = new Sub();
console.log(b.m(1), b.m(2.5), b.twice(3));
console.log(s.m('a'), s.twice('b'));
console.log(s.m({ toString() { return 'obj'; } }));
console.log(s.m(undefined));

// An override in a subclass module, reached through a receiver whose class
// the caller cannot narrow.
const all = [new Base(), new Loud(), new Sub()];
for (let i = 0; i < all.length; i++) console.log(all[i].describe(i === 1 ? 'x' : i));
