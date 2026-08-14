// Async generator with interleaved await and yield expressions,
// plus .throw() and .return() protocol tests.

async function delay(val) {
    return val;
}

async function* asyncYieldGen() {
    console.log("gen start");
    const a = await delay(100);
    console.log("awaited a:", a);
    const sent1 = yield (a + 1);
    console.log("received sent1:", sent1);
    const b = await delay(200);
    console.log("awaited b:", b);
    const sent2 = yield (b + sent1);
    console.log("received sent2:", sent2);
    return a + b;
}

const g = asyncYieldGen();
g.next().then((r1) => {
    console.log("step 1:", r1.value, r1.done);
    g.next(50).then((r2) => {
        console.log("step 2:", r2.value, r2.done);
        g.next(10).then((r3) => {
            console.log("step 3:", r3.value, r3.done);
        });
    });
});

async function* throwGen() {
    try {
        yield "try-1";
    } catch (e) {
        console.log("caught inside gen:", e);
        yield "recovered";
    }
    return "done-throw";
}

const gt = throwGen();
gt.next().then((r1) => {
    console.log("gt 1:", r1.value, r1.done);
    gt.throw("thrown-error").then((r2) => {
        console.log("gt 2:", r2.value, r2.done);
        gt.next().then((r3) => {
            console.log("gt 3:", r3.value, r3.done);
        });
    });
});

async function* returnGen() {
    yield "ret-1";
    yield "ret-2";
}

const gr = returnGen();
gr.next().then((r1) => {
    console.log("gr 1:", r1.value, r1.done);
    gr.return("early-return").then((r2) => {
        console.log("gr 2:", r2.value, r2.done);
        gr.next().then((r3) => {
            console.log("gr 3:", r3.value, r3.done);
        });
    });
});
