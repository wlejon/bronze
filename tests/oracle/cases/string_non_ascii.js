// Strings past ASCII. `Café` is Latin-1 in bronze's compact representation
// (docs/0004), so `length` counts CHARACTERS rather than the UTF-8 bytes the
// source file holds, and concatenation preserves them.

const str1 = "Café";
const str2 = " World";
const combined = str1 + str2;

console.log(str1.length);
console.log(str1.charCodeAt(3));
console.log(combined);
console.log(combined.length);
