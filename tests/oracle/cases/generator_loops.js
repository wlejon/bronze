// A `yield` inside a loop: the generator is RESUMED where it stopped, not
// restarted, and the loop's counter is the same counter on the other side of
// the call that resumed it.
//
// Every line of the expectation below was derived BY HAND from ECMA-262 before
// bronze was run on this file. The clauses that decide it:
//
// * 27.5.3.2 GeneratorResumeAbstractClosure: `next()` on a suspended generator
//   resumes its execution context at the point it was suspended. Nothing in the
//   clause re-enters the body at its top, so a `for` in the middle of its third
//   iteration continues into its fourth.
// * 14.7.4.9 ForBodyEvaluation and 14.7.4.7: the loop variable of a `for` with
//   a `let` head is one binding per iteration, copied forward by
//   CreatePerIterationEnvironment. Suspending inside the body does not end the
//   iteration, so the copy the body reads is the copy the increment updates.
// * 14.7.2.6 DoWhileLoopEvaluation / 14.7.3.6 WhileLoopEvaluation: the test is
//   evaluated afresh on every trip, so a body that mutates what the test reads
//   decides the trip count.
// * 14.9 / 14.8: `continue` and `break` complete the innermost iteration
//   statement, and a suspension in between changes nothing about which one that
//   is.
// * 13.2.4.1 and 25.1.4.2: spread drains the iterator to completion, so the
//   array it builds is exactly the yielded values in order, and the generator's
//   RETURN value is not one of them.

// --- a `for` whose counter survives every suspension ---------------------
function* count(n) {
    for (let i = 0; i < n; i++) {
        yield i;
    }
}

// Drained in one go, and then walked by hand one call at a time: the same
// four values either way.
console.log([...count(4)]);
const c = count(3);
console.log(c.next(), c.next(), c.next(), c.next());

// --- a `while` whose condition the body mutates ---------------------------
// 20 -> 10 -> 5 -> 2 -> 1, and the test fails at 1, so four yields and four
// steps. The tail `console.log` runs on the call that finds the loop finished,
// which is the call BEFORE spread's array is printed.
function* halve(n) {
    let steps = 0;
    while (n > 1) {
        n = Math.floor(n / 2);
        steps = steps + 1;
        yield n;
    }
    console.log('steps', steps);
}
console.log([...halve(20)]);

// --- `break` and `continue` still mean the loop they are written in -------
// i = 0 yields, 1 continues, 2 yields, 3 continues, 4 yields, 5 continues,
// 6 yields (6 > 6 is false), 7 continues, 8 breaks. Then the trailing yield.
function* evens(limit) {
    for (let i = 0; i < 100; i++) {
        if (i % 2 === 1) continue;
        if (i > limit) break;
        yield i;
    }
    yield 'done';
}
console.log([...evens(6)]);

// --- a loop nested in a loop: each resumes at its own position ------------
function* grid(n) {
    for (let row = 0; row < n; row++) {
        for (let col = 0; col < n; col++) {
            yield row * 10 + col;
        }
    }
}
console.log([...grid(3)]);

// --- the counter is per CALL, not per function ----------------------------
// Two live walks of one generator function interleave without sharing a step.
const p = count(3);
const q = count(3);
console.log(p.next().value, q.next().value, p.next().value, q.next().value);
console.log(p.next().value, p.next().done, q.next().value, q.next().done);
