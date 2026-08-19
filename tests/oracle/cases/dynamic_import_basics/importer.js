// A dynamic import from a NON-ENTRY module. The distinction is the whole point
// of this file: the linker renames a module's bindings by prefixing them with
// its id, and module 0 — the entry — is the one whose prefix is empty. So a
// dynamic import written in the entry exercised nothing about the rename, and
// the namespace local the linker invents for one written anywhere else kept its
// raw name while the declaration it was supposed to reach got the prefixed one.
// That mismatch is a ReferenceError, and only at the moment the program finally
// awaits that import.

export async function loadLazy(who) {
    const m = await import('./lazy.js');
    return m.describe(who) + " with " + m.lazyAnswer;
}

export async function loadLazyAgain() {
    // A second import of the SAME module reuses the one namespace local.
    const m = await import('./lazy.js');
    return m.lazyAnswer * 2;
}
