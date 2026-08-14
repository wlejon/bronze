// A `function` declaration directly in a switch case is hoisted across the
// WHOLE CaseBlock (ECMA-262 14.12.4, 14.2.3).
//
// CaseBlockEvaluation runs BlockDeclarationInstantiation over the entire
// CaseBlock before any clause is evaluated, and step 3.a.ii.1 of that
// algorithm creates and INITIALIZES a binding for every function declaration
// it finds — so unlike a `let` in a clause (whose TDZ `switch_case_lexical`
// pins), a function in a clause is callable from the first statement of the
// block, including from a clause textually above its own.
//
// This case is deliberately that shape: the call sits ABOVE the declaration,
// so it cannot pass on an implementation that merely evaluates declarations
// in order — only whole-CaseBlock hoisting answers it.

switch (1) {
  case 1:
    console.log(hoisted());
    function hoisted() { return 'hoisted'; }
    break;
}
