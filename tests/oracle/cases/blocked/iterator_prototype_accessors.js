// BLOCKED: `Iterator.prototype.constructor` and
// `Iterator.prototype[Symbol.toStringTag]` are plain absences today —
// `Iterator.prototype.constructor` is undefined and
// `Object.prototype.toString.call(Iterator.prototype)` is `[object Object]`.
//
// 27.1.4.3 and 27.1.4.4 make both of them ACCESSORS, which is unusual enough to
// be the whole content of this case. Every other built-in prototype carries
// `constructor` as a writable data property; %Iterator.prototype% does not,
// because it is a prototype user code is expected to subclass and the committee
// wanted a subclass's own `constructor` write to land on the subclass instead of
// mutating the shared object. So the setter is
// SetterThatIgnoresPrototypeProperties(this, %Iterator.prototype%, key, v):
//
//   - a write whose receiver IS %Iterator.prototype% throws a TypeError (step
//     2) rather than silently succeeding — the shared object is frozen in
//     practice without being frozen in fact;
//   - a write through any other receiver creates an OWN data property on that
//     receiver (step 4) and leaves %Iterator.prototype% untouched.
//
// bronze cannot express this yet: `rtDefineMethods` installs data properties on
// native prototype boxes and there is no native accessor-pair installer, so the
// pair is a missing piece of runtime machinery rather than two missing members.
// The iterator helpers themselves are implemented and pinned in
// cases/iterator_helpers.js — this is the metadata around them.
//
// The expectation below is what the pair owes when it lands.
console.log(Iterator.prototype.constructor === Iterator);
console.log(Object.prototype.toString.call(Iterator.prototype));
console.log(Iterator.prototype[Symbol.toStringTag]);

// Neither is enumerable, and neither is a data property.
console.log(Object.keys(Iterator.prototype).length);
const d = Object.getOwnPropertyDescriptor(Iterator.prototype, 'constructor');
console.log(typeof d.get, typeof d.set, d.enumerable, d.configurable, 'value' in d);

// Step 2: the home object refuses the write.
try {
  Iterator.prototype.constructor = 1;
  console.log('no throw');
} catch (e) {
  console.log('home', e.name);
}
try {
  Iterator.prototype[Symbol.toStringTag] = 'X';
  console.log('no throw');
} catch (e) {
  console.log('home tag', e.name);
}
console.log(Iterator.prototype.constructor === Iterator);

// Step 4: any other receiver gets an own data property, and the shared
// prototype still answers for everyone else.
const o = Object.create(Iterator.prototype);
o.constructor = 'mine';
console.log(o.constructor, Object.getOwnPropertyDescriptor(o, 'constructor').writable,
            Iterator.prototype.constructor === Iterator);
o[Symbol.toStringTag] = 'Mine';
console.log(Object.prototype.toString.call(o));

// The tag is inherited, so an object further down the chain that has its own
// shows its own — which is why a generator does not read as an Iterator.
function* g() {}
console.log(Object.prototype.toString.call(g()));
