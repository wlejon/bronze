// A data property of a module-scope object literal that a `delete` removes.
//
// 13.5.1.2 makes `delete Box.q` remove the own property, and 10.1.8.1 then
// answers a read of `q` off the prototype chain — `undefined` here, because
// `Object.prototype` has no `q`. So the value the literal wrote into `q` is not
// what `q` answers for the rest of the program, and neither the direct read nor
// the accessor whose body IS that read may keep it.
//
// Nothing in this program writes any property and nothing hands either literal
// to anything, so the `delete` is the ONLY reason a claim about `q` has to be
// refused — which is what makes the file a test of that reason rather than of
// some other one. The refusal is by NAME: `Shadow` is a second literal that is
// never deleted from, and its `q` must be refused too, because no syntactic
// scan can say which object a `delete o.q` reaches.

const Box = {
  q: 5,
  get p() { return this.q; }
};

const Shadow = {
  q: 'kept',
  get p() { return this.q; }
};

console.log('before        ' + Box.p + ' ' + Box.q + ' ' + Box['q']);

let hotBefore = 0;
for (let i = 0; i < 500; i++) {
  if (Box.p === 5) hotBefore++;
}
console.log('hot before    ' + hotBefore);

// 13.5.1.2: the property is configurable (7.3.5's default for a literal's data
// property), so [[Delete]] succeeds and the operator answers `true`.
console.log('delete        ' + (delete Box.q));

console.log('after         ' + Box.p + ' ' + Box.q + ' ' + typeof Box.p);

let hotAfter = 0;
for (let i = 0; i < 500; i++) {
  if (Box.p === undefined) hotAfter++;
}
console.log('hot after     ' + hotAfter);

// 13.5.1.2 again: deleting a property that is not there is not an error, and
// the operator still answers `true`.
console.log('delete again  ' + (delete Box.q) + ' ' + Box.q);

// Never deleted from, and it must answer its own value all the same.
console.log('shadow        ' + Shadow.p + ' ' + Shadow.q + ' ' + Shadow['q']);
