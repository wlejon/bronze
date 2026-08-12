// The `Error` family: what a constructed error carries, and how the three
// classes relate.
//
// ECMA-262 20.5.1.1 Error(message): "if NewTarget is undefined, let newTarget
// be the active function object" — so `Error("x")` builds the same thing as
// `new Error("x")` — and step 4 installs `message` as a NON-enumerable own
// data property when the argument is not undefined.
// 20.5.3.2 puts `message: ""` on Error.prototype and 20.5.3.3 `name: "Error"`.
// 20.5.6 makes each NativeError.prototype's [[Prototype]] Error.prototype and
// gives it its own `name`, which is why `instanceof Error` holds for all of
// them and `instanceof RangeError` does not hold for a TypeError.
//
// DELIBERATE DIVERGENCE FROM NODE: `console.log(err)` prints `Name: message`
// (the shape 20.5.3.4 Error.prototype.toString defines) and nothing else. node
// appends a captured stack trace, which is neither specified nor deterministic,
// and the oracle suite pins bytes.

var e = new Error("boom");
console.log(e.name, e.message);
console.log(e instanceof Error);
console.log(typeof e);

// No argument: `message` is left off the instance and the prototype's empty
// string shows through, so `in` finds it but `Object.keys` does not.
var bare = new Error();
console.log(bare.name, bare.message === "", "message" in bare);
console.log(Object.keys(e).length);

var t = new TypeError("bad");
console.log(t.name, t instanceof TypeError, t instanceof Error, t instanceof RangeError);
var r = new RangeError("oob");
console.log(r.name, r instanceof RangeError, r instanceof Error);

// Called without `new`.
console.log(Error("nonew").message);

console.log(e);
console.log(bare);
console.log([t, r]);

// A user subclass inherits `name` from Error.prototype, because it never
// writes one of its own.
class MyErr extends Error {}
var m = new MyErr("custom");
console.log(m instanceof MyErr, m instanceof Error, m.name, m.message);

try {
  throw new TypeError("thrown");
} catch (err) {
  console.log(err instanceof TypeError, err.message);
}

// The classic dispatch-on-class handler, which is the whole reason the
// prototype chain above has to be right.
function classify(v) {
  try {
    throw v;
  } catch (err) {
    if (err instanceof RangeError) return "range";
    if (err instanceof TypeError) return "type";
    if (err instanceof Error) return "error";
    return "other";
  }
}
console.log(classify(new RangeError("a")), classify(new TypeError("b")),
            classify(new Error("c")), classify("d"));
