// `this` inside an arrow inside an arrow. ECMA-262 10.2.1.1 gives an arrow
// function no `[[ThisMode]]` of its own: it never binds a receiver, so `this`
// in one resolves to the nearest ENCLOSING function that does, however many
// arrows are stacked in between.
//
// bronze gave every function body that mentions `this` under an arrow a
// receiver slot in its environment record — including an arrow's own record,
// which has no receiver to put in it. The slot was therefore filled with
// `undefined`, and an arrow nested one level deeper found that empty slot
// first and stopped there. One arrow worked; two did not.
//
// It matters here beyond its own sake: a generator's yielded expressions are
// desugared into an arrow, so a `yield` of anything containing an arrow that
// reads `this` sits at depth two by construction.
//
// What is pinned:
//
// 1. One, two and three arrows deep all see the method's receiver.
// 2. An arrow nested inside an ordinary FUNCTION expression stops at that
//    function, which does bind its own `this` — the nesting rule is about
//    what binds a receiver, not about depth.
// 3. A callback taking arrow — `map`, `forEach` — reads the enclosing
//    receiver from inside another arrow, which is the shape real code has.
// 4. The receiver is the CALLING one: the same method on two instances sees
//    two receivers, and neither leaks into the other.

class Scaled {
    constructor(scale, items) {
        this.scale = scale;
        this.items = items;
    }

    // 1 — one arrow, two arrows, three arrows.
    depths() {
        const one = () => this.scale;
        const two = () => {
            const inner = () => this.scale + 1;
            return inner();
        };
        const three = () => {
            const mid = () => {
                const deep = () => this.scale + 2;
                return deep();
            };
            return mid();
        };
        return [one(), two(), three()];
    }

    // 3 — an arrow callback inside an arrow.
    scaled() {
        const run = () => this.items.map((v) => v * this.scale);
        return run();
    }

    // The same, through two callback layers.
    pairs() {
        const run = () => this.items.map((v) => this.items.map((w) => v * w * this.scale));
        return run();
    }
}

const a = new Scaled(10, [1, 2]);
console.log(a.depths());
console.log(a.scaled());
console.log(a.pairs());

// 4 — two receivers, no leak.
const b = new Scaled(100, [3]);
console.log(b.depths(), b.scaled());
console.log(a.scaled());

// 2 — an ordinary function expression binds its own receiver, so an arrow
// inside it stops there rather than reaching the method.
class Host {
    constructor() {
        this.tag = 'host';
    }

    run() {
        const holder = {
            tag: 'holder',
            read: function () {
                const inner = () => this.tag;
                return inner();
            }
        };
        return [this.tag, holder.read()];
    }
}
console.log(new Host().run());

// The same rule at the top of a plain function: an arrow chain inside a
// function declaration reads that function's receiver.
function reader() {
    const outer = () => {
        const inner = () => this.mark;
        return inner();
    };
    return outer();
}
const carrier = { mark: 'M', reader: reader };
console.log(carrier.reader());
