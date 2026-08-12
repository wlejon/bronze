// BLOCKED: `unsupported construct: a 'function' declaration directly in a
// switch case (the switch body is one scope, so the declaration belongs to it
// rather than to the clause); wrap the case body in a block`.
//
// The sibling refusal for `let` and `const` in a case clause is gone — the
// temporal dead zone landed and made a jump past a lexical declaration well
// defined rather than impossible, which is what `switch_case_lexical` now
// pins. A `function` declaration in a clause was refused in the same edit,
// and for a different reason, so it gets its own case rather than riding on
// that one.
//
// The reason is hoisting, not scoping. ECMA-262 14.12.4 CaseBlockEvaluation
// runs BlockDeclarationInstantiation (14.2.3) over the WHOLE CaseBlock before
// any clause is evaluated, and step 3.a.ii.1 of that algorithm creates and
// INITIALIZES a binding for every function declaration it finds — so unlike a
// `let`, a function in a clause is callable from the first statement of the
// block, including from a clause textually above its own. bronze hoists
// function declarations from one statement list at a time and a CaseBlock's
// clauses are a list of lists, so the declaration below is never instantiated
// and the call above it would resolve to nothing.
//
// This case is deliberately the shape that separates the two mechanisms: the
// call sits ABOVE the declaration, so it cannot pass by accident on a
// implementation that merely evaluates declarations in order. When it does
// pass, promote it and rewrite this header to say what it pins.

switch (1) {
  case 1:
    console.log(hoisted());
    function hoisted() { return 'hoisted'; }
    break;
}
