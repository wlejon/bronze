// A data property of a module-scope object literal that a later statement
// writes: not a constant, and the read that follows the write must say so.
//
// `Store.q = 'second'` is 10.1.9.1 OrdinarySet on an own writable data
// property, so it stores, and every read after it — the direct one, the
// bracket one, and the accessor whose body is that read — answers the stored
// value. A compiler that took the initializer for the property's value would
// print `first` on the second line and would keep printing it in the loop.
//
// `Other` is the same claim through a receiver that has nothing to do with
// `Store`: the write is `Other.q = ...`, and it is the NAME `q` that a
// proof about `Store.q` has to answer for, because no syntactic scan can say
// which object an `o.q = v` reaches without an alias analysis.

const K = 'first';

const Store = {
  q: K,
  get p() { return this.q; }
};

const Other = {
  q: 'other'
};

console.log('before        ' + Store.p + ' ' + Store.q + ' ' + Store['q']);

let hotBefore = 0;
for (let i = 0; i < 500; i++) {
  if (Store.p === 'first') hotBefore++;
}
console.log('hot before    ' + hotBefore);

Store.q = 'second';

// 10.1.8.1: the getter runs and reads the property as it now stands.
console.log('after         ' + Store.p + ' ' + Store.q + ' ' + Store['q']);

let hotAfter = 0;
for (let i = 0; i < 500; i++) {
  if (Store.p === 'second') hotAfter++;
}
console.log('hot after     ' + hotAfter);

Other.q = 'changed';
console.log('other         ' + Other.q + ' ' + Store.q);

// A compound assignment is a read then a write, and both halves are real.
Store.q += '!';
console.log('compound      ' + Store.p + ' ' + Store.q);
