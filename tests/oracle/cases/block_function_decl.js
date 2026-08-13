"use strict";
// A function declaration written inside a BLOCK rather than directly in a
// script or function body.
//
// From ECMA-262:
//
// 1. 14.1: in strict code such a declaration is a lexical declaration of the
//    block. 14.2.2 BlockDeclarationInstantiation creates AND INITIALIZES it
//    when the block is entered, unlike a `let`, so a call written above the
//    declaration inside the same block works and there is no dead zone.
// 2. Being the block's own declaration, the name is visible nowhere else: it
//    does not reach the enclosing function, and an outer binding of the same
//    name is shadowed for the length of the block and uncovered after it.
// 3. It is an ordinary function in every other respect, closures included: a
//    block-scoped declaration captures the block's own `let` bindings.
//
// The first line is what makes rule 2 mean 14.1 and not something weaker.
// Annex B.3.3 gives SLOPPY code a second, legacy binding — the same name as a
// `var` in the enclosing function, so the block's function leaks out of it —
// and bronze implements 14.1 in both modes rather than that. Saying "use
// strict" here means the answers below are the ones every engine agrees on,
// and leaves bronze's sloppy-mode divergence to be argued about somewhere that
// is not a pinned expectation.

// Rule 1: called above its own declaration, inside the block.
{
    console.log(inner());
    function inner() { return 1; }
}

// Rule 2, the half that matters most: the outer function is uncovered again
// after the block, rather than having been replaced by the inner one.
function shadowed() { return "outer"; }
{
    function shadowed() { return "inner"; }
    console.log(shadowed());
}
console.log(shadowed());

// The body of an `if` is a block like any other.
if (true) {
    function branch() { return 2; }
    console.log(branch());
}

// Rule 2 again, from the other side: neither name escaped.
console.log(typeof inner, typeof branch);

// Rule 3: a closure over the block's `let`, called twice, seeing its own
// updates — so the binding it captured is the block's and not a copy.
{
    let n = 3;
    function bump() { n = n + 1; return n; }
    console.log(bump(), bump(), n);
}

// Two sibling blocks each declaring the same name declare two different
// functions, which is what "the block's own declaration" means.
{
    function which() { return "first"; }
    console.log(which());
}
{
    function which() { return "second"; }
    console.log(which());
}
