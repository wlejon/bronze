// Calls of a nested function declaration written above it or inside it: a
// recursive call, mutual recursion, a sibling declared further down, with
// too few and too many arguments, a default, a rest parameter, `arguments`,
// and a name that is assigned and so is not a known function at all.
function makeWalker() {
    let visited = 0;
    function walk(node, depth) {
        if (node.visible === false) return 0;
        visited++;
        let n = depth;
        const ch = node.children;
        for (let i = 0, l = ch.length; i < l; i++) n += walk(ch[i], depth + 1);
        return n;
    }
    return { run(root) { visited = 0; const d = walk(root, 0); return d + ':' + visited; } };
}
const mk = (n, w) => ({ visible: n % 5 !== 3, children: n > 0 ? Array.from({ length: w }, (_, i) => mk(n - 1, (w + i) % 3 + 1)) : [] });
const walker = makeWalker();
const tree = mk(6, 3);
let out = '';
for (let i = 0; i < 40; i++) out = walker.run(tree);
console.log(out);

function parity(n) {
    function isEven(k) { return k === 0 ? true : isOdd(k - 1); }
    function isOdd(k) { return k === 0 ? false : isEven(k - 1); }
    return isEven(n);
}
let evens = 0;
for (let i = 0; i < 300; i++) if (parity(i % 40)) evens++;
console.log(evens);

function renderer() {
    const log = [];
    function renderAll(items) { let s = 0; for (let i = 0; i < items.length; i++) s += renderOne(items[i], i); return s; }
    function renderOne(item, i, extra) { return extra === undefined ? scale(item) + offset(i, 2, 'dropped') : -1; }
    function scale(x, k = 3) { return x * k; }
    function offset(i, by) { return tail(i, by, i + by); }
    function tail(...rest) { return rest.length * 100 + rest[2]; }
    function count() { return arguments.length; }
    function short(a, b) { return b === undefined ? a : a + b; }
    function shorts() { return short(1) + short(1, 2) + count(1, 2, 3) + count(); }
    return { renderAll, shorts, log };
}
const r = renderer();
const items = [];
for (let i = 0; i < 50; i++) items.push(i * 0.5);
let acc = 0;
for (let i = 0; i < 400; i++) acc += r.renderAll(items) + r.shorts();
console.log(acc);

// A method's nested functions see no receiver of their own.
const holder = {
    tag: 'holder',
    run() {
        function self() { return this === undefined || this === globalThis ? 'none' : this.tag; }
        function ask(n) { return n > 0 ? ask(n - 1) : self() + '/' + self.call(holder); }
        return ask(3);
    },
};
console.log(holder.run());

// A name that is assigned is whatever was assigned last.
function swapped() {
    let calls = '';
    function first(n) { calls += 'f'; return n > 0 ? second(n - 1) : 0; }
    function second(n) { calls += 's'; return n > 0 ? first(n - 1) : 1; }
    const a = first(3);
    second = function (n) { calls += 'S'; return 7; };
    const b = first(3);
    return calls + a + b;
}
console.log(swapped());

// A parameter that is always a number, and a call that leaves it out.
function sums() {
    function tri(n, acc) { return n <= 0 ? acc : tri(n - 1, acc + n); }
    function half(n) { return n < 1 ? 0 : 1 + half(n / 2); }
    function odd(n, k) { return k === undefined ? odd(n, 1) : n + k; }
    let s = 0;
    for (let i = 0; i < 200; i++) s += tri(i % 30, 0) + half(i) + odd(i);
    return s;
}
console.log(sums());
