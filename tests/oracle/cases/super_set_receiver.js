// Where `super.k = v` puts the value, and what it does when the receiver
// refuses it.
//
// ECMA-262 13.3.7.1 makes a super reference one whose base is the HOME
// OBJECT's prototype and whose `this` value is the RECEIVER, and 6.2.5.6
// PutValue hands both to [[Set]]. 10.1.9.2 OrdinarySetWithOwnDescriptor then
// splits on what the walk found: an ACCESSOR (step 3) runs the setter with the
// receiver bound as `this`, and a DATA write (step 2) creates or updates an
// own property OF THE RECEIVER. The base object is where the walk STARTS, not
// where the value lands.
//
// Putting it on the base instead is a silent wrong answer of the worst kind:
// a class's base prototype is shared by every instance, so one object's write
// becomes every later object's initial value, and nothing in the program is
// wrong at the point the reader sees it. It also hides every refusal the
// receiver owed, because the base prototype is ordinarily extensible and
// writable when the receiver is not.
//
// A class body is strict code (15.7), and bronze has no other spelling of
// `super` — `super` in an object literal's method is refused by name — so a
// refused super write is always a TypeError here and there is no sloppy half
// to pin.
//
// What each line pins:
//
// 1. The write creates an own property of the INSTANCE, and leaves the base
//    prototype and the derived prototype without one.
// 2. A second instance of the same class does not see the first one's write.
//    This is the consequence that makes the first line matter.
// 3. An inherited SETTER still runs, and still runs with the instance as
//    `this` — step 3 is unchanged by any of this, and a fix that routed every
//    super write to the receiver as data would have broken it.
// 4. An inherited non-writable data property refuses the write (step 2.a
//    reaches the base's descriptor) rather than being shadowed.
// 5. A frozen receiver refuses: its own property is non-writable, so step 2.a
//    answers false and PutValue raises. The value does not change and nothing
//    lands on the base.
// 6. A non-extensible receiver refuses a NEW key, which is the other of the
//    two data refusals — different test, same answer.
// 7. A compound `super.k += 1` reads through the chain and writes to the
//    receiver, so it refuses on the same terms an ordinary one does.
// 8. A write that is NOT refused still goes through: the strict flag decides
//    what happens to a refusal, not whether every super write is one.

class Base {}
class Sub extends Base {
  constructor() {
    super();
  }
  put(v) {
    super.k = v;
  }
}

const first = new Sub();
first.put(5);
console.log(first.k, Object.prototype.hasOwnProperty.call(first, "k"));
console.log(
  Object.prototype.hasOwnProperty.call(Base.prototype, "k"),
  Object.prototype.hasOwnProperty.call(Sub.prototype, "k")
);

const second = new Sub();
console.log(second.k);
second.put(6);
console.log(first.k, second.k);

class SetterBase {
  set w(v) {
    this.seen = v * 2;
  }
}
class SetterSub extends SetterBase {
  go() {
    super.w = 4;
  }
}
const withSetter = new SetterSub();
withSetter.go();
console.log(
  withSetter.seen,
  Object.prototype.hasOwnProperty.call(SetterBase.prototype, "seen")
);

class LockedBase {}
Object.defineProperty(LockedBase.prototype, "ro", {
  value: 1,
  writable: false,
  enumerable: true,
  configurable: false,
});
class LockedSub extends LockedBase {
  go() {
    super.ro = 9;
  }
}
const inheritor = new LockedSub();
try {
  inheritor.go();
  console.log("inherited-readonly no throw");
} catch (e) {
  console.log("inherited-readonly", e instanceof TypeError, e.name);
}
console.log(inheritor.ro, Object.prototype.hasOwnProperty.call(inheritor, "ro"));

class FrozenSub extends Base {
  constructor() {
    super();
    this.k = 1;
    Object.freeze(this);
  }
  go() {
    super.k = 9;
  }
  add() {
    super.fresh = 9;
  }
  bump() {
    super.k += 1;
  }
}
const frozen = new FrozenSub();
try {
  frozen.go();
  console.log("frozen-existing no throw");
} catch (e) {
  console.log("frozen-existing", e instanceof TypeError, e.name);
}
try {
  frozen.add();
  console.log("frozen-new no throw");
} catch (e) {
  console.log("frozen-new", e instanceof TypeError, e.name);
}
try {
  frozen.bump();
  console.log("frozen-compound no throw");
} catch (e) {
  console.log("frozen-compound", e instanceof TypeError, e.name);
}
console.log(frozen.k, frozen.fresh);
console.log(Object.prototype.hasOwnProperty.call(Base.prototype, "fresh"));

const open = new Sub();
open.put(3);
console.log(open.k);
