// Basic dynamic import() tests with static and dynamic specifiers

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

run();
