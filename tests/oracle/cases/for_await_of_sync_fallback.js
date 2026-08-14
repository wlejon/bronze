// for-await-of fallback to synchronous iterables (Array, Set, sync generator, custom Symbol.iterator)

function* syncGen() {
    yield "a";
    yield "b";
    yield "c";
}

async function testArraySyncFallback() {
    console.log("=== sync array ===");
    const arr = [100, 200, 300];
    for await (const x of arr) {
        console.log("arr item:", x);
    }
}

async function testSetSyncFallback() {
    console.log("=== sync set ===");
    const s = new Set();
    s.add("alpha");
    s.add("beta");
    for await (const x of s) {
        console.log("set item:", x);
    }
}

async function testSyncGenFallback() {
    console.log("=== sync gen ===");
    for await (const x of syncGen()) {
        console.log("gen item:", x);
    }
}

async function testCustomSyncIterable() {
    console.log("=== custom sync iterable ===");
    const custom = {
        [Symbol.iterator]() {
            let i = 0;
            return {
                next() {
                    if (i < 2) {
                        return { value: "custom-" + (++i), done: false };
                    }
                    return { value: undefined, done: true };
                }
            };
        }
    };
    for await (const x of custom) {
        console.log("custom:", x);
    }
}

async function main() {
    await testArraySyncFallback();
    await testSetSyncFallback();
    await testSyncGenFallback();
    await testCustomSyncIterable();
}

main();
