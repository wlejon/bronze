// A function's `name` and `length`, and the one attribute that decides what an
// assignment to either does.
//
// ECMA-262 10.2.9 SetFunctionName and 10.2.10 SetFunctionLength both define the
// property with { [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]:
// true }. Non-writable is the operative half: 10.1.9.2 [[Set]] answers false
// for it, and 13.15.2 PutValue step 6.d turns that false into a TypeError only
// for a STRICT reference — so `f.name = "x"` in sloppy code stores nothing and
// raises nothing, and in strict code it throws.
//
// Configurable is the other half, and it is why the pair is not simply frozen:
// `Object.defineProperty` can still redefine either, which is how a library
// gives a wrapper the name of the function it wraps.
//
// Both SPELLINGS of the assignment are pinned, because they are two lowerings
// of one operation: `f.name = v` names the key at compile time and `f[k] = v`
// does not, and a refusal built into one of them is not a refusal. The read
// path consults a function's statics table first, so a write that reached that
// table would be a `name` the language says cannot be set and the program can
// nonetheless observe.
//
// The last two lines are the boundary: `name` and `length` are ordinary
// writable properties of everything that is not a function, which is what three
// .js relies on every time it writes `this.name` on a scene object.

function attempt(fn) {
    try {
        return fn();
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}
function show(o, k) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    return d ? ('v=' + String(d.value) + ' w=' + d.writable + ' e=' + d.enumerable +
                ' c=' + d.configurable)
             : 'none';
}
// A COMPUTED store, whose key is not known where the store is written.
function put(o, k, v) { o[k] = v; return o; }
function putStrict(o, k, v) { 'use strict'; o[k] = v; return 'stored'; }

// 1-3. The two properties and their three attributes. `length` is 10.2.10's
// count of parameters before the first default or rest.
function two(a, b) {}
console.log('1', two.name, two.length);
console.log('2', show(two, 'name'));
console.log('3', show(two, 'length'));

// 4-5. An anonymous function expression and an arrow get their names from the
// binding they initialise (8.6.2 NamedEvaluation), so they have the pair too.
const anon = function () {};
const arrow = (a) => a;
console.log('4', anon.name, anon.length);
console.log('5', arrow.name, arrow.length);

// 6-7. Sloppy assignment: discarded, in both spellings.
const f1 = function () {};
f1.name = 'renamed';
f1.length = 9;
console.log('6', f1.name, f1.length);

const f2 = function () {};
put(f2, 'name', 'renamed');
put(f2, 'length', 9);
console.log('7', f2.name, f2.length);

// 8-10. Strict assignment: 13.15.2 step 6.d, in both spellings.
console.log('8', attempt(() => { 'use strict'; const g = function () {}; g.name = 'x'; return 'stored'; }));
console.log('9', attempt(() => putStrict(function () {}, 'name', 'x')));
console.log('10', attempt(() => putStrict(function () {}, 'length', 9)));

// 11. Nothing above changed an attribute either.
console.log('11', show(f1, 'name'), show(f2, 'length'));

// 12. Every OTHER key on a function is an ordinary writable own property, in
// both spellings — the refusal is about two names and not about the receiver.
const f3 = function () {};
put(f3, 'tag', 't');
f3.other = 'o';
console.log('12', f3.tag, f3.other, f3.name);

// 13. Configurable, so `defineProperty` reaches what assignment cannot.
const f4 = function () {};
console.log('13', attempt(() => { Object.defineProperty(f4, 'name', { value: 'given' }); return f4.name; }));

// 14-15. A class is a function (15.7.14), so its constructor carries the same
// pair on the same terms.
class K { static m() {} }
console.log('14', K.name, K.length, show(K, 'name'));
console.log('15', attempt(() => { 'use strict'; K.name = 'other'; return 'stored'; }), K.name);

// 16. A method's name is the property key it was defined under.
const holder = { meth(a, b, c) {} };
console.log('16', holder.meth.name, holder.meth.length);

// 17-18. And the boundary: on a plain object both names are ordinary data
// properties, and an array's `length` is 10.4.2's own writable one.
const plain = { name: 'a', length: 1 };
plain.name = 'b';
put(plain, 'length', 2);
console.log('17', plain.name, plain.length);

const arr = [1, 2, 3];
put(arr, 'length', 1);
console.log('18', arr.length, JSON.stringify(arr));
