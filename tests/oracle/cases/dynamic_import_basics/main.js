// Basic dynamic import() tests with static and dynamic specifiers

import { loadLazy, loadLazyAgain } from './importer.js';
import { makeRunner } from './deep.js';

async function run() {
    console.log("start import");
    const mod = await import("./helper.js");
    console.log("imported answer:", mod.answer);
    console.log("imported greet:", mod.greet("Bronze"));
    console.log("imported math:", mod.math.add(10, 20));
    console.log("imported tag:", Object.prototype.toString.call(mod));

    try {
        const dynamicSpecifier = "nonexistent.js";
        await import(dynamicSpecifier);
    } catch (e) {
        console.log("caught unresolvable import:", e);
    }
}

async function runFromOtherModules() {
    console.log("non-entry import:", await loadLazy("bronze"));
    console.log("non-entry import again:", await loadLazyAgain());
    await new Promise(function (resolve) {
        makeRunner("R")('lazy', function (text) {
            console.log("nested import:", text);
            resolve();
        });
    });
}

// Chained rather than started side by side: what is pinned here is that each
// import resolves to the right namespace, not how two async functions
// interleave their microtasks.
run().then(runFromOtherModules);
