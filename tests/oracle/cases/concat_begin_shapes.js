// What a `+` chain's FIRST step builds, across the shapes its operands take.
//
// A chain of three or more `+` starts with `concat.begin`, which for two
// Latin-1 strings mints the accumulator straight from the allocation window
// and copies both texts in, with room for the operands still to come; every
// other pair (a number on either side, a UTF-16 string, an accumulator
// already begun by an inner chain, an empty operand) takes the general path.
// The text and the length must be the same whichever path a step took, for
// short operands and long ones, and when the allocation window fills in the
// middle of a run of chains.

const out = [];
function note(label, s) {
    out.push(label + ':' + s.length + ':' + s);
}

const empty = '';
const a = 'alpha';
const wide = 'héllo 世界';
const latinHigh = 'café';
const long = 'x'.repeat(70);

// Two Latin-1 strings, then one, two and many more operands.
note('ll3', a + ' ' + 'bravo');
note('ll4', a + '-' + a + '-' + a);
note('ll9', a + 1 + a + 2 + a + 3 + a + 4 + a);
// An empty operand first, second, both.
note('e1', empty + a + '!');
note('e2', a + empty + '!');
note('e3', empty + empty + a);
note('e4', empty + empty + empty);
// Latin-1 above ASCII stays one byte a unit.
note('hi', latinHigh + '/' + latinHigh);
// UTF-16 on the left, on the right, and arriving third.
note('wl', wide + a + '.');
note('wr', a + wide + '.');
note('w3', a + '.' + wide);
note('w4', a + '.' + a + wide + a);
// A number on either side of the first step.
note('nl', 12 + a + 'z');
note('nr', a + 12.5 + 'z');
note('nn', 1 + 2 + a + 3 + 4);
// Operands longer than the room the first step guesses.
note('lg', a + ':' + long + long + '|' + long);
note('lg2', long + long + a + long);
// A chain whose first operand is itself a finished chain.
const inner = a + '+' + a;
note('in', inner + '=' + inner + '?');
// null, undefined and booleans in the first pair.
note('nu', null + a + undefined + true);

// Many chains back to back, so some begin as the allocation window fills:
// every one must come out whole.
let bad = 0, total = 0;
const words = ['alpha', 'bravo', 'charlie', 'delta', 'echo'];
for (let i = 0; i < 200000; i++) {
    const w = words[i % 5];
    const s = w + ' ' + w + '/' + (i % 1000);
    total += s.length;
    if (s.length !== w.length * 2 + 2 + String(i % 1000).length || s.charCodeAt(w.length) !== 32) bad++;
}
out.push('many:' + bad + ':' + total);

console.log(out.join('\n'));
