// The call shapes around a module-scope object literal that are NOT `X.m(a)`.
//
// A method of such a literal can be reached in more ways than one, and the
// ways differ in what `this` is and in whether the function is being called at
// all. Each of them is settled by the language and by nothing about the
// literal, so this case pins them beside the plain call — which appears here
// too, before and after every one of the others, because the interesting
// failure is a plain call that starts answering differently once a program
// has read the method as a value or constructed with it.

const Space = {
  enabled: true,
  base: 'B',
  convert: function (v, from, to) {
    if (this.enabled === false || from === to || !from || !to) {
      return v;
    }
    return v + '|' + from + '>' + to;
  },
  toWorking: function (v, from) {
    return this.convert(v, from, this.base);
  },
  boxed: function (n) {
    if (n === 0) return { k: 'zero' };
    return { k: 'other' };
  }
};

// 13.3.3 and 13.3.2 name the same property, and 13.3.6.1 makes the base of
// either the this value.
console.log('bracket fast  ' + Space['toWorking']('c', 'B'));
console.log('bracket slow  ' + Space['toWorking']('c', 'x'));

// A call whose RESULT is the base of an optional chain. The chain's short
// circuit is a question about that result, not about the call.
console.log('chain hit     ' + Space.boxed(0)?.k);
console.log('chain miss    ' + Space.boxed(1)?.k);
console.log('chain deep    ' + Space.boxed(0)?.k?.length);

// A call in the ARGUMENTS of a link of someone else's chain.
const holder = { take: function (x) { return 'took:' + x; } };
console.log('chain arg     ' + holder?.take(Space.toWorking('c', 'x')));

// `X.m` read as a VALUE. The function escapes; the object does not — nothing
// here can reach `Space` itself. A call through the alias is 13.3.6.1's
// receiver-less form, so `this` is undefined, and module code is strict
// (11.2.2), so it stays undefined and the first member read on it throws.
const detached = Space.toWorking;
try {
  console.log('detached      ' + detached('c', 'x'));
} catch (e) {
  console.log('detached      ' + e.name);
}
// 20.2.3.3 and 20.2.3.1 hand the receiver back explicitly.
console.log('via call      ' + detached.call(Space, 'c', 'x'));
console.log('via apply     ' + detached.apply(Space, ['c', 'B']));

// `new X.m()` runs the body with a fresh ordinary object as `this`, and
// 10.2.2's step 12 answers with the object the body RETURNED when it returned
// one — which is a plain literal here, so it is not an instance of anything
// but Object.
const made = new Space.boxed(0);
console.log('new typeof    ' + typeof made);
console.log('new instance  ' + (made instanceof Space.boxed));
console.log('new key       ' + made.k);

console.log('still fast    ' + Space.toWorking('c', 'B'));
console.log('still slow    ' + Space.toWorking('c', 'x'));
