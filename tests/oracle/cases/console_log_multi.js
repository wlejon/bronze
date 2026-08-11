// console.log with more than one argument.
//
// node's console.log formats each argument exactly as it would a single one
// and joins the results with a SINGLE space, then writes one newline. So a
// top-level string is still raw and a string inside a container is still
// quoted, and `-0` is still `-0` — the inspect format of docs/0013 is not
// re-decided per argument count, which is why there is one formatter and this
// case shares every rule with `print_containers` and `print_primitives`.
//
// Zero arguments is a bare newline: there is nothing to join, and node still
// terminates the line.
//
// (This is console.log, not util.format: bronze does no `%s`/`%d`
// substitution, and node only applies it when the first argument is a string
// containing one. No case here uses a percent sign.)

console.log(1, 2, 3);
console.log('a', 'b');
console.log('n =', 42);
console.log(true, null, undefined);
console.log([1, 2], { a: 1 });
console.log('x', ['y']);
console.log(-0, 0);
console.log(1.5, 'ok');
console.log('one');
console.log();

// The arguments are ordinary expressions, evaluated left to right before
// anything is written.
let seq = '';
function m(tag, value) { seq = seq + tag; return value; }
console.log(m('1', 'a'), m('2', 'b'), m('3', 'c'));
console.log(seq);

// Nesting depth and quoting are per argument, exactly as for one argument.
console.log([[1, [2, [3, [4]]]]], 'after');
console.log({ s: "it's" }, "it's");

// A void call's value is undefined, and undefined is a printable argument.
function nothing() {}
console.log(nothing(), 'and', nothing());
