// String.prototype.concat called on coercible non-string this values

console.log(String.prototype.concat.call(123, "abc", 456));
console.log(String.prototype.concat.call(true, "!", false));
console.log(String.prototype.concat.call({ toString() { return "[custom]"; } }, " appended"));

function catchErr(fn) {
  try {
    fn();
    return "no-throw";
  } catch (e) {
    return e instanceof TypeError ? "TypeError" : e.constructor.name;
  }
}

console.log(catchErr(() => String.prototype.concat.call(null, "a")));
console.log(catchErr(() => String.prototype.concat.call(undefined, "a")));