// A dynamic import whose specifier is a TEMPLATE LITERAL — the shape the
// three.js editor uses to pick a per-geometry parameter panel. There is no
// string to resolve at compile time, so what bronze resolves instead is the
// head and tail as a glob over one directory: every file that matches joins
// the graph, and the call becomes a lookup keyed by the string the program
// finally computes. A miss rejects, which is what a browser does with a 404.

async function load(type) {
    const mod = await import(`./panels/Panel.${type}.js`);
    return mod.label + ': ' + mod.describe(2);
}

// A second pattern over the SAME directory, to check that two patterns get
// their own tables and that a file reached twice is still one module.
async function labelOnly(type) {
    const mod = await import(`./panels/Panel.${type}.js`);
    return mod.label;
}

async function run() {
    console.log(await load('Box'));
    console.log(await load('Sphere'));
    console.log('label only:', await labelOnly('Sphere'));

    // The glob skips README.txt: the tail is `.js` and a name must end with it.
    try {
        await load('../README');
    } catch (e) {
        console.log('escape refused:', e.message);
    }

    // A name the directory does not hold.
    try {
        await load('Missing');
    } catch (e) {
        console.log('miss rejects:', e.message);
    }

    // The specifier is ToString()ed before it is looked up, per 16.2.1.8.
    const coerced = { toString() { return 'Box'; } };
    console.log('coerced:', await load(coerced));
}

run();
