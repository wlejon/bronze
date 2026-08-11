// Automatic semicolon insertion (ECMA-262 12.10). Every statement below is
// terminated by a rule rather than by a semicolon, and the restricted
// productions decide their meaning by line break alone: the same source with
// the newlines removed would mean something else, which is why this is
// pinned rather than left to read as formatting.

// Inserted before a token on a later line.
let a = 1
let b = 2
console.log(a + b)

// Inserted before the `}` that closes the block, and at the end of input.
function g() { return 7 }
console.log(g())

// NOT inserted: the `+` continues the expression, so this is one addition
// and not a declaration followed by unary plus.
const c = 1
+ 2
console.log(c)

// NOT inserted: a member access continues an expression the same way.
const s = "abc"
  .toUpperCase()
console.log(s)

// `return` is restricted. The expression on the next line is a separate,
// unreachable statement, and the function returns undefined.
function noValue() {
  return
  1
}
console.log(noValue())

// A block comment that spans lines IS a line terminator (ECMA-262 12.4), so
// this return is restricted for the same reason.
function alsoNoValue() {
  return /*
  */ 1
}
console.log(alsoNoValue())

// Postfix `++` is restricted. The operator on the next line belongs to the
// next statement as a PREFIX increment, so `e` keeps `d`'s old value and `d`
// is incremented afterwards.
let d = 5
let e = d
++d
console.log(d)
console.log(e)

// `break` and `continue` are restricted: the name below each of them is a
// statement, not a label.
let sum = 0
for (let i = 0; i < 5; i++) {
  if (i === 2) continue
  if (i === 4) break
  sum += i
}
console.log(sum)

// A do-while may drop its terminating semicolon outright (ECMA-262 14.7.2).
let k = 0
do { k++ } while (k < 3)
console.log(k)

// Terminated by the end of input.
console.log("end")
