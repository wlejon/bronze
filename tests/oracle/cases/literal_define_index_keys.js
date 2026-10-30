// Integer-like keys are names like any other on an ordinary object, and a
// literal DEFINES them: a setter Object.prototype carries for "0" or "1" is
// not run by `{ 0: v }`, `{ "1": v }`, `{ [2]: v }`, a spread of an array or
// a string, or a computed class field. The log is a string, not an array, so
// that nothing here writes an array index while the setters are installed.

let log = "";
function note(s) {
  log += (log === "" ? "" : ",") + s;
}
function flush() {
  const s = log;
  log = "";
  return s;
}
function f(n) {
  note("f" + n);
  return n;
}
function own(o, k) {
  const d = Object.getOwnPropertyDescriptor(o, k);
  if (d === undefined) return String(k) + ":absent";
  if ("get" in d) return String(k) + ":accessor";
  return String(k) + ":" + String(d.value) + (d.writable ? "w" : "-") + (d.enumerable ? "e" : "-") +
    (d.configurable ? "c" : "-");
}

for (const k of ["0", "1", "2"]) {
  Object.defineProperty(Object.prototype, k, {
    set(v) {
      note("setter " + k + " " + v);
    },
    configurable: true,
  });
}

const numeric = { 0: f(10), "1": f(11), [2]: f(12) };
console.log("literal", flush(), own(numeric, "0"), own(numeric, "1"), own(numeric, "2"));

const fromArray = { ...["a", "b"] };
console.log("spread array", flush(), own(fromArray, "0"), own(fromArray, "1"));

const fromString = { ..."xy" };
console.log("spread string", flush(), own(fromString, "0"), own(fromString, "1"));

class Indexed {
  [0] = "field 0";
  [1 + 1] = "field 2";
}
const ix = new Indexed();
console.log("class", flush(), own(ix, "0"), own(ix, "2"));

// An assignment, for contrast, runs the inherited setter.
const plain = {};
plain[1] = "assigned";
console.log("assignment", flush(), own(plain, "1"));

for (const k of ["0", "1", "2"]) {
  delete Object.prototype[k];
}
console.log("restored", ({})[0], ({})[1]);
