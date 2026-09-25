// A name a function declares shadows the enclosing binding of that name from
// the function's first statement, not from the statement that declares it, so
// a read BEFORE the declaration must not take the outer binding's type. The
// shape is three.js r160's minified WebGLState: `$(1)` above its nested
// `function $`, in a factory holding `const $ = 33776` that a sibling closure
// reads (which makes the outer `$` a captured cell with a proven type). Typed
// as the outer number, the call throws "a number is not a function".
(function () {
    "use strict";
    const $ = 33776;
    const x = 5;
    const y = 7;
    function outerReads() { return $ + x + y; }

    // A hoisted function declaration, called above it.
    function callsAbove() {
        const r = $(1);
        function $(e) { return "fn:" + e; }
        return r;
    }
    console.log(callsAbove());

    // Its type above the declaration.
    function typeofAbove() {
        const t = typeof $;
        function $() {}
        return t;
    }
    console.log(typeofAbove());

    // Stored as a value above the declaration, then called.
    function storedAbove() {
        const api = { cull: $ };
        function $(e) { return e * 2; }
        return api.cull(21);
    }
    console.log(storedAbove());

    // A `var` holds undefined until its assignment runs.
    function varAbove() {
        const before = typeof x;
        const plus = x + 1;
        var x = "s";
        return before + " " + plus + " " + typeof x;
    }
    console.log(varAbove());

    // A function declared in a block is that function from the top of the
    // block.
    function blockFunction() {
        {
            const t = typeof y;
            function y() {}
            return t;
        }
    }
    console.log(blockFunction());

    // A loop whose body reads the hoisted var before assigning it.
    function varInLoop() {
        const seen = [];
        for (let i = 0; i < 3; i++) {
            seen.push(typeof x);
            var x = i;
        }
        return seen.join(",");
    }
    console.log(varInLoop());

    console.log(outerReads());
})();
