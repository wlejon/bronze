// A loop whose body is an if-without-else nested in one arm of an if/else
// whose other arm returns. The IL lists the blocks so that a block's
// dominator can come AFTER it in the list; lowering in list order then read
// a value (here `leading`'s join) before the block defining it was lowered,
// and the MIR verifier rejected the function with "operand is null".

function leadingZero(value, n) {
  let num = 0;
  let leading = 0;
  for (let i = 0; i < n; i++) {
    const ch = value.charCodeAt(i);
    if (ch >= 48) {
      if (num === 0) leading = ch;
      num = num * 10 + (ch - 48);
    } else {
      return false;
    }
  }
  return num >= 0 && leading === 48;
}
console.log(leadingZero("1234", 4), leadingZero("0", 1), leadingZero("12/4", 4));

// The shape it was found in: a dotted-quad check.
function isIPv4(s) {
  let parts = 0;
  let digits = 0;
  let num = 0;
  let leading = 0;
  for (let i = 0; i < s.length; i++) {
    const ch = s.charCodeAt(i);
    if (ch === 46) {
      if (digits === 0 || num > 255) return false;
      if (digits > 1 && leading === 48) return false;
      parts++;
      num = 0;
      digits = 0;
    } else if (ch >= 48 && ch <= 57) {
      if (digits === 0) leading = ch;
      num = num * 10 + (ch - 48);
      digits++;
    } else {
      return false;
    }
  }
  if (digits === 0 || num > 255) return false;
  if (digits > 1 && leading === 48) return false;
  return parts === 3;
}
for (const s of ["1.2.3.4", "255.255.255.255", "256.1.1.1", "01.2.3.4", "1.2.3", "1.2.3.a", "0.0.0.0"]) {
  console.log(s, isIPv4(s));
}
