// A constructor that lets `this` escape (`const self = this`, `init(this)`)
// and then calls something may have fields installed through the alias in an
// order the class layout cannot see (class_layout.h `openTail`): the layout
// stops at that call, and reads and writes of every field — before, after and
// through the alias — still see what the language says.

function install(o, n) {
    for (let i = 0; i < n; i++) o["dyn" + i] = i * 10;
}

class Renderer {
    constructor(n) {
        this.a = 1;
        this.b = "two";
        const self = this;
        function init() {
            self.caps = { max: 8 };
            self.state = [n];
            install(self, n);
        }
        init();
        this.c = 3;
        this.d = self.caps.max + this.a;
    }
    get sum() { return this.a + this.c + this.d + this.state[0]; }
    bump() { this.c += 1; this.d *= 2; return this.sum; }
}

class Sub extends Renderer {
    constructor(n) {
        super(n);
        this.own = "own" + n;
    }
    get sum() { return super.sum + this.own.length; }
}

class Passed {
    constructor(f) {
        this.x = 1;
        f(this);
        this.y = 2;
    }
}

function show(o) {
    console.log(JSON.stringify(Object.keys(o)), JSON.stringify(o));
}

let total = 0;
for (let r = 0; r < 300; r++) {
    const n = r % 4;
    const o = r % 3 === 0 ? new Sub(n) : new Renderer(n);
    total += o.bump() + o.sum + o.c + o.d;
    if (r < 8) show(o);
}
console.log("total", total);

for (let r = 0; r < 200; r++) {
    const p = new Passed(r % 2 ? (t) => { t.z = r; t.y0 = -1; } : () => {});
    total += p.x + p.y + (p.z || 0);
    if (r < 3) show(p);
}
console.log("total", total);
