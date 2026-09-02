// The three shapes of a dynamic import() specifier, and what the module graph
// does with each. A CONSTANT string is followed at compile time and resolves
// to the compiled namespace. A template bounded by a module extension is a
// glob over one directory, and a string it did not find rejects. Everything
// else — a variable, or a template whose interpolation is followed by no
// extension (`../../${path}`) — is NOT followed: nothing joins the graph for
// it, the compiler warns at the site, and at run time the call is the host's
// to answer or rejects with a TypeError. That holds even when the string the
// program computes names a module that is on disk: the graph was fixed when
// the program was compiled, and no file is read after that.

async function run() {
    const mod = await import('./helper.js');
    console.log('constant:', mod.answer);

    const name = 'helper.js';
    try {
        await import(`./${name}`);
    } catch (e) {
        console.log('unbounded template:', e instanceof TypeError, e.message);
    }

    const spec = './helper.js';
    try {
        await import(spec);
    } catch (e) {
        console.log('variable specifier:', e instanceof TypeError, e.message);
    }

    // Bounded, so it IS a glob — over a directory that does not exist, which
    // is an empty table rather than an error, and a miss in it.
    const stem = 'helper';
    try {
        await import(`./missing/${stem}.js`);
    } catch (e) {
        console.log('empty glob:', e instanceof TypeError, e.message);
    }
}

run();
