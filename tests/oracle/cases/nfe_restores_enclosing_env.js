// A named function expression is created inside a one-slot record of its own
// (15.2.5), and that record must not stay behind as the ENCLOSING function's
// environment once the expression is lowered: the statements after it read
// the enclosing function's slots, not the record's one slot.

function collect(root) {
    const acc = [];
    (function walk(node) {
        acc.push(node.name);
        if (node.children) node.children.forEach(walk);
    })(root);
    return acc.join(',');
}
console.log('nested:', collect({ name: 'r', children: [{ name: 'a' }, { name: 'b', children: [{ name: 'c' }] }] }));

// Direct recursion, the same enclosing-slot read afterwards.
function countdown(n) {
    const seen = [];
    (function step(k) { seen.push(k); if (k > 0) step(k - 1); })(n);
    return seen.join(' ');
}
console.log('direct:', countdown(3));

// Two records in one function, and a capture written after both.
function twice() {
    let total = 0;
    (function a(i) { total += i; if (i > 0) a(i - 1); })(2);
    (function b(i) { total += i * 10; if (i > 0) b(i - 1); })(1);
    total += 100;
    return total;
}
console.log('two records:', twice());

// At the top level: a module-scope binding read after the expression.
const label = 'top';
const result = (function fact(n) { return n <= 1 ? 1 : n * fact(n - 1); })(4);
console.log(label + ':', result);

// The record is invisible outside the expression.
const f = function inner() { return typeof inner; };
console.log('outside:', typeof inner, 'inside:', f());
