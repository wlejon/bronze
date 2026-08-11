// The numeric literal forms.
//
// Derived from ECMA-262 12.9.3. The MV of a HexIntegerLiteral, an
// OctalIntegerLiteral and a BinaryIntegerLiteral is its digits read in base
// 16, 8 and 2; the radix letter is case-insensitive and so are the hex
// digits. A NumericLiteralSeparator contributes nothing to the MV, so
// `1_000_000` and `1000000` denote the same Number — separators are grouping
// for the reader and not a different literal. An ExponentPart multiplies by
// the stated power of ten, and a DecimalLiteral may begin with the `.`.
//
// Every value below is printed through ToString(Number), whose rules are
// already pinned by `number_formatting`; what this case pins is what the
// literals DENOTE. `0xFF` used to lex as `0` followed by an identifier
// `xFF`, so every form here was a compile error naming nothing.
//
// Legacy octal (`017`) is deliberately absent: it is a SyntaxError in strict
// mode and would otherwise be a silent wrong answer (15 or 17, and nothing
// in the source says which), so bronze diagnoses it by name. The same goes
// for a misplaced separator. Both are pinned as errors in tests/lex.

console.log(0xFF);
console.log(0xff);
console.log(0X10);
console.log(0xA_B);
console.log(0o17);
console.log(0O777);
console.log(0b1010);
console.log(0B1111_0000);

console.log(1_000_000);
console.log(1_0.2_5);
console.log(1_000.000_1);

console.log(1e3);
console.log(1E3);
console.log(1.5e-3);
console.log(2e-7);
console.log(1e21);
console.log(1e1_0);
console.log(.5);
console.log(.25e2);

// The forms compose with the operators, and a hex literal is a Number like
// any other: `|` is ToInt32 of it (docs/0015 decision 1), so the sign bit of
// 0x80000000 is observable.
console.log(0xFF + 1);
console.log(0b1 << 4);
console.log(0x7FFFFFFF);
console.log(0xFFFFFFFF);
console.log(0x80000000 | 0);
console.log(0xFF & 0x0F);

// Separators change nothing about identity.
console.log(1_000 === 1000);
console.log(0b1111_1111 === 255);
console.log(0o1_0 === 8);
