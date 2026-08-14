// Basic async generator creation, execution, and protocol checks.

async function* simpleGen() {
    console.log("start");
    yield 10;
    console.log("after 10");
    yield 20;
    console.log("after 20");
    return 30;
}

const g = simpleGen();

console.log(g[Symbol.asyncIterator]() === g);
console.log(Object.prototype.toString.call(g));

g.next().then((r1) => {
    console.log("r1:", r1.value, r1.done);
    g.next().then((r2) => {
        console.log("r2:", r2.value, r2.done);
        g.next().then((r3) => {
            console.log("r3:", r3.value, r3.done);
            g.next().then((r4) => {
                console.log("r4:", r4.value, r4.done);
            });
        });
    });
});

// Object literal method and class method
const obj = {
    async *method() {
        yield "obj-yield";
    }
};

class Cls {
    async *method() {
        yield "cls-yield";
    }
}

const gObj = obj.method();
gObj.next().then((res) => {
    console.log("obj method:", res.value, res.done);
});

const gCls = new Cls().method();
gCls.next().then((res) => {
    console.log("cls method:", res.value, res.done);
});
