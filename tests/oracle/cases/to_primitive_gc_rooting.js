// ToPrimitive with a conversion method that ALLOCATES, at every site that
// performs one. bronze's collector moves objects, so a `valueOf` that builds
// strings and arrays is a collection in the middle of an operator — and every
// operand the operator was still holding has to be reached through a root
// afterwards, not through the pointer it had before.
//
// The comparison of two objects is the sharp case: it converts twice, and the
// SECOND conversion can move what the first one returned. A stale first result
// is not a crash — it is a comparison against whatever now lives at that
// address — so the answers below are the test, and running this case under
// BRONZE_GC_STRESS is what makes them mean anything.
//
// Everything printed is derived from data the conversion methods built, so a
// pointer that went stale shows up as wrong bytes rather than as silence.

function repeated(tag, n) {
    const parts = [];
    for (let i = 0; i < n; i++) parts.push(tag + i);
    return parts.join('-');
}

// A `toString` that allocates an array, a string per element and a join.
function heavyString(tag) {
    return { toString() { return repeated(tag, 30); } };
}

// A `valueOf` that allocates and then answers a number derived from what it
// built, so the number itself proves the work happened.
function heavyNumber(tag, base) {
    return {
        valueOf() {
            const parts = [];
            for (let i = 0; i < 40; i++) parts.push({ tag, text: tag + i });
            return base + parts.length;
        },
    };
}

// Two objects, each converting to a freshly built string. The first result must
// survive the second conversion.
const p = heavyString('p');
const q = heavyString('q');
console.log(p < q, q < p, p <= q, p >= q);
console.log(String(p).length, String(q).length, String(p) === String(q));

// The same shape with numbers, through all four operators.
const low = heavyNumber('low', 100);
const high = heavyNumber('high', 200);
console.log(low < high, low > high, low <= high, low >= high);
console.log(low * 1, high * 1, high - low);

// `==` restarts, and each restart is another conversion that can collect.
console.log(low == 140, high == 240, low == high);
console.log(heavyString('z') == repeated('z', 30));

// `+` on both operands, where the left result is a heap string held across the
// right conversion.
console.log((heavyString('a') + heavyString('b')).length);
console.log(heavyString('c') + '|' + heavyNumber('d', 0));

// A property KEY whose conversion allocates, against a receiver that must not
// move out from under the write. The receiver is given its own properties so a
// stale header would corrupt something visible.
const receiver = { alpha: 1, beta: 2, gamma: 3 };
const bigKey = heavyString('k');
const keyText = repeated('k', 30);
receiver[bigKey] = 'stored';
console.log(receiver[bigKey], receiver[keyText], receiver.alpha, receiver.gamma);
console.log(Object.keys(receiver).join(','), bigKey in receiver);
delete receiver[bigKey];
console.log(keyText in receiver, Object.keys(receiver).length);

// An ARRAY receiver whose key conversion allocates, and whose elements have to
// still be there afterwards.
const list = [1, 2, 3, 4];
console.log(list[{ toString() { return repeated('', 0) === '' ? '2' : '0'; } }]);
list[{ toString() { return '3'; } }] = repeated('v', 10);
console.log(list[3].length, list.length, list[0]);

// A typed array element write: 10.4.5.5 runs ToNumber on the value, so the view
// is reached through a root after the conversion rather than before it.
const view = new Float64Array(4);
for (let i = 0; i < 4; i++) view[i] = heavyNumber('v', i * 10);
console.log(view[0], view[1], view[2], view[3]);

// A conversion inside a loop, so the collector runs many times with live
// operands on either side of it.
let total = 0;
let text = '';
for (let i = 0; i < 25; i++) {
    const item = heavyNumber('loop', i);
    total += item * 2;
    if (item < 1000) text += 'x';
}
console.log(total, text.length);

// A conversion that throws after allocating, so the unwind path also runs with
// a moved heap behind it.
let caught = 0;
for (let i = 0; i < 10; i++) {
    const angry = {
        valueOf() {
            const junk = [];
            for (let j = 0; j < 20; j++) junk.push('j' + j);
            throw new Error('after ' + junk.length);
        },
    };
    try {
        angry * 2;
    } catch (e) {
        if (e.message === 'after 20') caught++;
    }
}
console.log(caught);

// A builtin's numeric ARGUMENT is ToNumber too, so an object there is a user
// `valueOf` running in the middle of a method that was holding the receiver's
// elements pointer. Each of these took the header before the conversion and now
// takes it after; the answers are what says so.
function allocatingIndex(n) {
    return {
        valueOf() {
            const junk = [];
            for (let i = 0; i < 30; i++) junk.push('j' + i);
            return n + junk.length - 30;
        },
    };
}

const nums = [1, 2, 3, 4, 5];
console.log(nums.indexOf(4, allocatingIndex(0)), nums.lastIndexOf(2, allocatingIndex(4)));
console.log(nums.includes(3, allocatingIndex(1)), nums.at(allocatingIndex(-1)));
console.log(nums.slice(allocatingIndex(1), allocatingIndex(3)).join(','), nums.join('-'));

const filled = [0, 0, 0, 0];
filled.fill(9, allocatingIndex(1), allocatingIndex(3));
console.log(filled.join(','));

const copied = [1, 2, 3, 4, 5];
copied.copyWithin(allocatingIndex(0), allocatingIndex(3));
console.log(copied.join(','));

const spliced = [1, 2, 3, 4, 5];
console.log(spliced.splice(allocatingIndex(1), allocatingIndex(2)).join(','), spliced.join(','));

console.log('ab'.repeat(allocatingIndex(3)), 'abcdef'.slice(allocatingIndex(1), allocatingIndex(3)));

const typed = new Int32Array([1, 2, 3, 4]);
typed.fill(7, allocatingIndex(1), allocatingIndex(3));
console.log(typed.join(','), typed.at(allocatingIndex(-1)), typed.indexOf(7, allocatingIndex(0)));
