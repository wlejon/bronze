// A method called on a STRING receiver is cached against `String.prototype`
// (bronze_abi.h's method-site contract, the primitive form), and the cache
// reads the intrinsic's slot live — so every way a program can change what
// `s.charCodeAt` means must be seen by a site that already warmed.

function sum(s) { let t = 0; for (let i = 0; i < s.length; i++) t += s.charCodeAt(i); return t; }
function up(s) { let t = ''; for (let i = 0; i < 3; i++) t += s.toUpperCase(); return t; }
function poly(x) { return x.charCodeAt(0); }

console.log(sum('abc'), sum('hello'), sum(''));
console.log(up('ab'));

// The slot overwritten in place: no shape change, still honoured.
const native = String.prototype.charCodeAt;
String.prototype.charCodeAt = function (i) { return 1000 + i; };
console.log(sum('abc'), sum('hello'));
String.prototype.charCodeAt = native;
console.log(sum('abc'));

// An accessor installed over a method.
Object.defineProperty(String.prototype, 'toUpperCase', { get() { return () => 'G'; }, configurable: true });
console.log(up('ab'));

// A method added by a program, then deleted.
String.prototype.twice = function () { return this + '' + this; };
function tw(s) { return s.twice(); }
console.log(tw('ab'), tw('c'));
delete String.prototype.twice;
try { tw('ab'); console.log('MUST NOT PRINT'); } catch (e) { console.log(e instanceof TypeError); }

// A site that sees strings and objects by turns.
const o = { charCodeAt: () => 7 };
console.log(poly('z'), poly(o), poly('q'), poly(o), poly('r'));

// Number receivers keep taking the ordinary path beside string ones.
function fixed(x) { return x.toFixed(1); }
console.log(fixed(1.25), fixed(2), fixed(-0.5));

// `length` on a string beside `length` on an array and a typed array at one
// site, and the index properties the string exotic object synthesises.
function len(x) { return x.length; }
console.log(len('four'), len([1, 2]), len(new Float64Array(3)), len(''), len('\u{1F600}'));
const s = 'hé';
console.log(s[0], s[1], s[2], s['1'], 'abc'.length + 'de'.length);

// A prototype the string's chain reaches beyond String.prototype.
Object.prototype.shout = function () { return String(this).toUpperCase() + '!'; };
function shout(s) { return s.shout(); }
console.log(shout('ab'), shout('x'));
delete Object.prototype.shout;
