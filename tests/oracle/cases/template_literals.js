// Template literals. The lexer has no backtick at all today, so this is
// `unrecognized character '`'`. Substitutions are ToString of the value,
// a newline inside the literal is a real newline, and the escapes are the
// same ones a quoted literal resolves.
const name = "World";
const n = 3;
console.log(`Hello, ${name}!`);
console.log(`1 + 2 = ${1 + 2}`);
console.log(`${n} item${n === 1 ? "" : "s"}`);
console.log(`nested ${`inner ${n}`}`);
console.log(`a
b`.length);
console.log(`no substitution`);
console.log(`${undefined} ${null} ${true}`);
console.log(`\n`.length);
console.log(`$ {not} \${escaped}`);
