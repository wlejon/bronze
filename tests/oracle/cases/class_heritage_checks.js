// 15.7.14 steps 8.e-8.h: the heritage must be null or a constructor whose
// `prototype` is an object or null, checked once at class definition.

function report(label, make) {
    try {
        const C = make();
        console.log(label, 'defined', typeof C);
        return C;
    } catch (e) {
        console.log(label, e.constructor.name);
        return undefined;
    }
}

function F(x) {
    this.x = x;
}
F.prototype.hello = function () {
    return 'hello ' + this.x;
};

// A bound function is a constructor with no `prototype` own property:
// heritage.prototype is undefined, neither an object nor null.
report('bound', () => class extends F.bind(null) {});
report('bound-named', () => {
    const bound = F.bind(null, 1);
    return class X extends bound {};
});

// Not a constructor at all.
report('arrow', () => class extends (() => 1) {});
report('method', () => class extends ({ m() {} }).m {});
report('async', () => class extends (async function () {}) {});
report('generator', () => class extends (function* () {}) {});
report('object', () => class extends ({}) {});
report('number', () => class extends (5) {});
report('string', () => class extends ('s') {});
report('undefined', () => class extends undefined {});
report('builtin-method', () => class extends Math.max {});

// A prototype that is a primitive, or null.
{
    function P() {}
    const withProto = (v) =>
        new Proxy(P, {
            get(t, k) {
                return k === 'prototype' ? v : Reflect.get(t, k);
            },
        });
    report('primitive-proto', () => class extends withProto(5) {});
    report('undefined-proto', () => class extends withProto(undefined) {});
    const C = report('null-proto', () => class extends withProto(null) {});
    console.log(Object.getPrototypeOf(C.prototype) === null);
}

// An accessor `prototype` is read once, at definition.
{
    let reads = 0;
    function Q() {}
    const q = new Proxy(Q, {
        get(t, k) {
            if (k === 'prototype') reads++;
            return Reflect.get(t, k);
        },
    });
    const C = report('proxy', () => class extends q {});
    new C();
    new C();
    console.log('prototype reads', reads);
}

// extends null: defined; constructing throws at super(); a constructor that
// returns an object works.
{
    const C = report('null', () => class extends null {});
    console.log(Object.getPrototypeOf(C.prototype) === null, Object.getPrototypeOf(C) === Function.prototype);
    try {
        new C();
        console.log('null construct: no throw');
    } catch (e) {
        console.log('null construct', e.constructor.name);
    }
    const R = class extends null {
        constructor() {
            return Object.create(R.prototype);
        }
    };
    const r = new R();
    console.log(r instanceof R, Object.getPrototypeOf(Object.getPrototypeOf(r)) === null);
}

// super() through a bound [[Prototype]] constructs its target with the bound
// arguments first, on the derived receiver.
{
    class S extends F {
        constructor(y) {
            super(y);
            this.y = y;
        }
    }
    Object.setPrototypeOf(S, F.bind(null, 42));
    const s = new S(7);
    console.log(s.x, s.y, s instanceof S, s instanceof F, s.hello());
}
