// ECMA-262 7.1.19 ToPropertyKey with an OBJECT key: ToPrimitive with hint
// STRING, and then ToString — unless the result is a Symbol, which already IS a
// property key and must not be stringified.
//
// Hint STRING is the whole difference from every other conversion site: `o[k]`
// asks `k.toString` before `k.valueOf`, where `o < k` and `o * k` ask the other
// way round. An object defining both really does name a different property than
// it compares as.
//
// The conversion runs at the ENTRY of each operation — the read, the write, the
// delete, `in`, and the two computed definition forms — so it happens exactly
// once and before the receiver is touched. That ordering is what makes a key
// whose `toString` allocates safe: the receiver is rooted across the call
// instead of being held as a raw pointer through it.

const key = { toString() { return 'k'; } };
const store = {};
store[key] = 1;
console.log(store.k, store[key], store['k'], 'k' in store, Object.keys(store).join(','));

// Hint string, so `toString` wins over `valueOf` — the reverse of what `*` does
// with the same object.
const both = { toString() { return 'name'; }, valueOf() { return 7; } };
const bag = {};
bag[both] = 'by string';
bag[7] = 'by number';
console.log(bag[both], bag[7], bag.name, both * 1);
console.log(Object.keys(bag).join('|'));

// `valueOf` answering with an object falls through to `toString`, and a
// `valueOf` that answers a primitive is still not consulted first here.
const fallback = { valueOf() { return {}; }, toString() { return 'f'; } };
const held = {};
held[fallback] = 2;
console.log(held.f, fallback in held);

// `Symbol.toPrimitive` wins and receives "string".
const hints = [];
const hinted = { [Symbol.toPrimitive](hint) { hints.push(hint); return 'h'; } };
const box = {};
box[hinted] = 3;
console.log(box.h, box[hinted], hinted in box, hints.join(','));

// 7.1.19 step 2: a Symbol result is the key itself, not its description.
const tag = Symbol('tag');
const symKeyed = { [Symbol.toPrimitive]() { return tag; } };
const target = {};
target[symKeyed] = 'symbol slot';
target['Symbol(tag)'] = 'string slot';
console.log(target[tag], target[symKeyed], target['Symbol(tag)']);
console.log(Object.keys(target).join(','));

// An ARRAY receiver: the converted key names an element when it is a canonical
// index and a named property otherwise.
const arr = [10, 20, 30];
console.log(arr[{ toString() { return '1'; } }], arr[{ toString() { return 'length'; } }]);
arr[{ toString() { return '0'; } }] = 99;
console.log(arr[0], arr.length);

// `delete o[k]` and `k in o` run the same conversion.
const doomed = { gone: 1, kept: 2 };
delete doomed[{ toString() { return 'gone'; } }];
console.log('gone' in doomed, 'kept' in doomed, ({ toString() { return 'kept'; } }) in doomed);

// The computed definition forms — a class method and an accessor — take it too.
const methodKey = { toString() { return 'greet'; } };
const accessorKey = { toString() { return 'twice'; } };
class Widget {
    [methodKey]() { return 'hi'; }
    get [accessorKey]() { return this.n * 2; }
}
const w = new Widget();
w.n = 21;
console.log(w.greet(), w.twice, Object.keys(w).join(','));

// An object literal's computed key is the same conversion.
const literal = { [key]: 'lit', [both]: 'lit2' };
console.log(literal.k, literal.name);

// The key is converted exactly ONCE per operation, and before the value is
// stored.
const calls = [];
const counted = { toString() { calls.push('t'); return 'c'; } };
const once = {};
once[counted] = once[counted];
console.log(calls.length, 'c' in once);

// A conversion that throws leaves the property untouched and is catchable.
const angry = { toString() { throw new Error('key'); } };
const safe = { a: 1 };
try { safe[angry] = 2; } catch (e) { console.log('set', e.message); }
try { safe[angry]; } catch (e) { console.log('get', e.message); }
try { delete safe[angry]; } catch (e) { console.log('delete', e.message); }
console.log(Object.keys(safe).join(','));

// 7.1.1.1 step 4 in a key position.
const noPrimitive = { valueOf() { return {}; }, toString() { return {}; } };
try { safe[noPrimitive]; } catch (e) { console.log(e instanceof TypeError, e.message); }

// The rest of 7.1.19's step 2, for the primitives that are not strings. Each
// names a PROPERTY — `a[true]` is `a.true`, not element 1 — so an array carries
// them beside its elements rather than in them.
const mixed = {};
mixed[true] = 'yes';
mixed[null] = 'nothing';
mixed[undefined] = 'missing';
mixed[1.5] = 'fractional';
console.log(mixed.true, mixed.null, mixed.undefined, mixed['1.5']);
console.log(Object.keys(mixed).join(','));

const withNamed = [10, 20];
withNamed[true] = 'flag';
withNamed[null] = 'nil';
console.log(withNamed[true], withNamed.true, withNamed[null], withNamed.length);
console.log(withNamed[0], withNamed[1], Object.keys(withNamed).join(','));
console.log(withNamed[false], withNamed[undefined]);
