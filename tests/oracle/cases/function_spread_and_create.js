// Two `Object` members that took the process down on an ordinary receiver.
//
// `{ ...fn }` and `Object.assign({}, fn)` are 7.3.25 CopyDataProperties over a
// function, whose own ENUMERABLE keys are exactly its statics — `prototype`,
// `length` and `name` are non-enumerable (10.2.4, 10.2.9, 10.2.10), so a spread
// never had to reach them. The statics live in the side object every function
// keeps them in, which is a property table like any other; the refusal claimed
// there was none. Library code copies a function's statics routinely
// (`Object.assign({}, Component)`), so this was a `fatal` a program reached by
// writing correct JavaScript.
//
// `Object.create(fn)` is still a gap — a prototype is walked by every read that
// misses, and bronze answers a function's members from a table beside the value
// rather than from a shape a walk can step through. What changed is that it is
// a catchable TypeError instead of a process abort: the receiver is a value the
// program was holding, so a reflective walk that meets one must be able to
// report it and carry on.
function Widget(a, b) {
  return a + b;
}
Widget.create = function () { return 1; };
Widget.VERSION = "2";

const copied = Object.assign({}, Widget);
console.log(JSON.stringify(Object.keys(copied)), copied.VERSION, typeof copied.create);
console.log(JSON.stringify({ ...Widget }));
// The three header properties are non-enumerable, so neither form takes them.
console.log("prototype" in copied, "length" in copied, "name" in copied);
// A function with no statics spreads to nothing rather than to its header.
console.log(JSON.stringify({ ...function bare() {} }), JSON.stringify(Object.assign({}, () => 1)));
// A class's statics are the same table.
class Box {
  static kind = "box";
  static make() { return new Box(); }
}
console.log(JSON.stringify(Object.keys({ ...Box })));

// Catchable, with the reason in the message.
for (const proto of [Widget, [1, 2]]) {
  try {
    Object.create(proto);
    console.log("no throw");
  } catch (e) {
    console.log(e instanceof TypeError, e.message.indexOf("Object.create with") === 0);
  }
}
// The two that DO work are pinned beside them, so a fix for the above cannot
// quietly change what a plain prototype and `null` do.
console.log(Object.getPrototypeOf(Object.create(Widget.prototype)) === Widget.prototype);
console.log(Object.getPrototypeOf(Object.create(null)));
