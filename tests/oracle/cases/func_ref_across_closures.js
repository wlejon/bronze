// A top-level `function` mentioned as a VALUE inside a nested closure, and
// then again in the enclosing function.
//
// The reference is memoized per IL function (`functionRefMap_`), and the memo
// holds an instruction result id, which only means anything inside the
// function that emitted it. Lowering a closure clears the memo on the way in;
// this is the case that proves it is also restored on the way out. Before it
// was, `useIt` below resolved to whichever instruction of `main` happened to
// have the id the nested `func.ref @scale` was given — the anonymous function
// itself — so line 3 ran twice and the last line called nothing.

function scale(k) { return k * 2; }
function useIt(f, x) { return f(x); }

(function () { console.log('in ' + useIt(scale, 4)); })();
console.log('out ' + useIt(scale, 3));

// The same shape one level deeper, and with the outer mention FIRST so the
// memo is populated before the closure is lowered rather than by it.
function outer() {
    console.log('outer ' + useIt(scale, 5));
    function inner() { console.log('inner ' + useIt(scale, 6)); }
    inner();
    console.log('outer again ' + useIt(scale, 7));
}
outer();

// A method body is lowered through the same path.
class Holder {
    run() { return useIt(scale, 8); }
}
console.log('method ' + new Holder().run());
console.log('after method ' + useIt(scale, 9));
