// A regex literal is a fresh object each time it is evaluated, with its own
// lastIndex and properties, whatever the runtime shares underneath.
function make() { return /a(b)?/g; }
const first = make(), second = make();
console.log(first === second, first.source === second.source, first.flags, second.flags);
first.lastIndex = 5;
first.extra = 'mine';
console.log(second.lastIndex, second.extra, first.lastIndex, first.extra);
console.log(first instanceof RegExp, Object.getPrototypeOf(second) === RegExp.prototype);

// Evaluated in a loop: state never leaks from one evaluation to the next.
const hits = [];
for (let i = 0; i < 4; i++) {
    const re = /o/g;
    hits.push(re.lastIndex, re.test('foo'), re.lastIndex);
}
console.log(hits.join(' '));

// The same source with different flags, and different sources with the same.
function flagged(n) { return n === 0 ? /x/ : n === 1 ? /x/i : n === 2 ? /x/gi : /y/gi; }
for (let round = 0; round < 2; round++) {
    for (let n = 0; n < 4; n++) {
        const re = flagged(n);
        console.log(re.source, re.flags, re.global, re.ignoreCase, re.test('X'), re.test('Y'));
    }
}

// `new RegExp` with literal strings behaves the same way.
function built() { return new RegExp('(\\d+)px', 'g'); }
const b1 = built(), b2 = built();
b1.exec('12px 34px');
console.log(b1 === b2, b1.lastIndex, b2.lastIndex, b2.exec('12px 34px')[1], String(b1));
console.log(new RegExp('a', 'g').flags, new RegExp('a', '').flags, new RegExp('', 'y').source, new RegExp('a/b', 'm').source);

// Errors are still errors every time.
for (let i = 0; i < 2; i++) {
    try { new RegExp('(', 'g'); } catch (e) { console.log(e instanceof SyntaxError); }
    try { new RegExp('a', 'gg'); } catch (e) { console.log(e instanceof SyntaxError); }
    try { new RegExp('a', 'q'); } catch (e) { console.log(e instanceof SyntaxError); }
}

// Non-string arguments take the general path.
const src = /k+/y;
const copy = new RegExp(src, 'g');
console.log(copy.source, copy.flags, copy === src, new RegExp(src).flags, new RegExp(12, undefined).source);
console.log(new RegExp({ toString() { return 'obj'; } }, { toString() { return 'i'; } }).toString());
console.log(RegExp('z', 'u').unicode, RegExp('z', 'u') === RegExp('z', 'u'));

// Results from one literal's matches are independent arrays.
function measure(font) { return /(\d+(?:\.\d+)?)px/.exec(font); }
const m1 = measure('bold 26px Arial'), m2 = measure('10.5px serif'), m3 = measure('none');
m1[1] = 'changed';
console.log(m1[1], m2[1], m2.index, m2.input, m3, measure('bold 26px Arial')[1]);

// A subclass and a patched-in own property do not confuse evaluation.
class Mine extends RegExp { get mine() { return true; } }
const mine = new Mine('a', 'g');
console.log(mine.mine, mine.flags, mine instanceof Mine, /a/g.mine);
