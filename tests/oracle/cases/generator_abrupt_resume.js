// The two abrupt resumptions: `return(v)` closes a generator early, and
// `throw(e)` raises the exception AT the suspension point, where the code
// around it can catch it.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.3 `Generator.prototype.return`: resumes the generator with a RETURN
//   completion carrying `value`. A generator suspended at a `yield` therefore
//   completes as if a `return value;` had been written there, and the answer is
//   `{ value: <v>, done: true }`.
// * 27.5.3.3 via GeneratorResumeAbstractClosure: if the generator is
//   `suspendedStart`, the state becomes `completed` and the body NEVER RUNS.
//   Nothing the body would have printed is printed.
// * If the generator is already `completed`, `return(v)` answers
//   `{ value: v, done: true }` without running anything.
// * 27.5.3.4 `Generator.prototype.throw`: resumes with a THROW completion, so
//   the exception is raised at the suspension point. A `try` written AROUND the
//   `yield` in the body sees it (14.15.3 CatchClauseEvaluation), and the
//   generator can carry on afterwards.
// * If no handler in the body catches it, the exception propagates out of the
//   `throw()` call to the caller, and the generator is left `completed`
//   (27.5.1.4: an abrupt completion of the body sets the state to completed).
// * 27.5.3.4 on a `completed` generator throws the exception straight back at
//   the caller without entering the body.
// * 27.5.3.2 step 2: resuming a generator whose state is `executing` is a
//   TypeError. Its message is implementation-defined, so what is pinned is the
//   constructor, not the text.

function* count3() {
    yield 1;
    yield 2;
    yield 3;
}

// --- `return(v)` on a suspended generator ---------------------------------
const a = count3();
console.log(a.next());
console.log(a.return(99));
console.log(a.next());

// --- `return(v)` on one that has not started ------------------------------
// 'body ran' is never printed: the body does not run at all.
function* traced() {
    console.log('body ran');
    yield 1;
}
const t = traced();
console.log(t.return('never'));
console.log(t.next());

// --- `return(v)` on one that is already finished --------------------------
const d = count3();
d.next();
d.next();
d.next();
console.log(d.next());
console.log(d.return('late'));

// --- `throw(e)` caught inside the body ------------------------------------
function* guarded() {
    try {
        yield 'first';
        yield 'unreached';
    } catch (err) {
        console.log('caught', err);
        yield 'recovered';
    }
    yield 'after';
}
const g = guarded();
console.log(g.next());
console.log(g.throw('boom'));
console.log(g.next());
console.log(g.next());

// --- `throw(e)` with no handler in the body -------------------------------
function* naked() {
    yield 1;
    yield 2;
}
const n = naked();
console.log(n.next());
try {
    n.throw('escaped');
} catch (err) {
    console.log('escaped to the caller:', err);
}
// The failed resumption left it completed, not suspended at `yield 1`.
console.log(n.next());

// --- `throw(e)` on a completed generator ----------------------------------
const done = naked();
done.next();
done.next();
console.log(done.next());
try {
    done.throw('late throw');
} catch (err) {
    console.log('threw', err);
}
console.log(done.next());

// --- resuming one that is already running ---------------------------------
// The inner `next` finds the generator `executing` and throws a TypeError,
// which nothing in the body catches, so it propagates out of the OUTER `next`.
let running = null;
function* reenter() {
    running.next();
    yield 'unreached';
}
running = reenter();
try {
    running.next();
} catch (err) {
    console.log('re-entry is a TypeError:', err instanceof TypeError);
}
console.log(running.next());
