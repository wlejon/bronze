// for-await-of loop over async generators and objects with Symbol.asyncIterator.

async function* asyncNumbers() {
    yield 1;
    yield 2;
    yield 3;
}

async function testAsyncGenLoop() {
    console.log("=== async gen loop ===");
    for await (const x of asyncNumbers()) {
        console.log("item:", x);
    }
}

async function testCustomAsyncIterable() {
    console.log("=== custom async iterable ===");
    const custom = {
        [Symbol.asyncIterator]() {
            let i = 0;
            return {
                async next() {
                    if (i < 3) {
                        return { value: ++i * 10, done: false };
                    }
                    return { value: undefined, done: true };
                }
            };
        }
    };

    for await (const num of custom) {
        console.log("custom item:", num);
    }
}

async function testBreak() {
    console.log("=== break in for-await ===");
    for await (const x of asyncNumbers()) {
        console.log("break item:", x);
        if (x === 2) break;
    }
}

async function main() {
    await testAsyncGenLoop();
    await testCustomAsyncIterable();
    await testBreak();
}

main();
