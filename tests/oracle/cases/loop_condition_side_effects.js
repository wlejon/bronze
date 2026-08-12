// A loop condition is an EXPRESSION, and an expression may assign. This case
// pins what the assignment on the LAST evaluation of it — the one that ends the
// loop — is worth after the loop has ended.
//
// ECMA-262 evaluates the test in the same place for all three forms:
// 14.7.4.8 ForBodyEvaluation step 3.a for `for`, 14.7.2 step 2.a for `while`,
// 14.7.3 step 2.d for `do`/`while`. In each, the expression is evaluated to
// completion and only THEN is ToBoolean asked; a false answer ends the
// iteration statement, it does not undo the evaluation that produced it. So the
// final test's writes are observable after the loop, and they are the writes a
// counted loop's body never makes: `for (let i = 0; (seen = i), i < 3; i++)`
// leaves `seen` at 3 while the body only ever ran with 0, 1 and 2.
//
// That off-by-one is the whole reason this case exists. Lowering gives the exit
// block a parameter per loop variable and the exiting branch an argument list to
// fill them; collect that list before lowering the condition and every line
// below still prints the value from the START of the final header block, which
// for four of them is the value from the previous iteration and for the rest is
// the initial one. Nothing about the loop's shape is wrong in that IL — the
// arguments simply name the wrong values, which is why only a program that
// assigns in the condition can see it.
//
// What each line holds:
//
// 1. The three forms with a comma-expression condition, which is the plain
//    shape: the assignment runs on every evaluation including the last.
//    `while` and `do`/`while` also print their counter, so the case says what
//    the loop variable is worth beside what the condition wrote.
// 2. A condition that assigns ONLY on the evaluation that ends the loop — the
//    right-hand side of a `||` whose left side is the real test, so the write
//    happens on the exiting trip and on no other. This is the exit edge alone;
//    line 1 could be carried by a body edge that happened to be right.
// 3. A condition that writes a variable declared OUTSIDE the loop and the head
//    binding the loop CARRIES, in one expression. The two travel by different
//    routes — one is a plain loop parameter, the other is the counter the
//    update expression would otherwise own — and `mirror` reports the carried
//    one's final value, which no other line can reach.
// 4. The body reading what the condition wrote, so that the final `30` is
//    pinned against the trail of what the body saw: the exit value is one step
//    beyond the last body value, not equal to it.
// 5. `break`, which leaves by the body's edge rather than the condition's. It
//    is here so that fixing the condition's edge cannot be done by making every
//    edge into the exit block re-collect from the wrong place.
// 6. Nested loops, where the inner condition writes a variable the OUTER loop
//    also carries — so the value has to survive the inner exit edge and then the
//    outer one.
//
// `for-of` and `for-in` are absent by construction: their heads hold no test
// expression (14.7.5.7 asks the iterator), so there is nothing for an exit edge
// to drop.

let forSeen = -1;
for (let i = 0; (forSeen = i), i < 3; i++) {}
console.log(forSeen);

let whileSeen = -1, j = 0;
while (((whileSeen = j), j < 3)) { j++; }
console.log(whileSeen, j);

let doSeen = -1, k = 0;
do { k++; } while (((doSeen = k), k < 3));
console.log(doSeen, k);

let forLast = "none";
for (let i = 0; i < 3 || ((forLast = "exit"), false); i++) {}
console.log(forLast);

let whileLast = "none", m = 0;
while (m < 3 || ((whileLast = "exit"), false)) { m++; }
console.log(whileLast, m);

let doLast = "none", n = 0;
do { n++; } while (n < 3 || ((doLast = "exit"), false));
console.log(doLast, n);

let outerVar = "start", mirror = -1;
for (let i = 0; (outerVar = "i" + i), (i = i + 1), (mirror = i), i < 4; ) {}
console.log(outerVar, mirror);

let trail = "", bodySeen = -1;
for (let i = 0; (bodySeen = i * 10), i < 3; i++) { trail += bodySeen + ";"; }
console.log(trail, bodySeen);

let broke = -1;
for (let i = 0; (broke = i), i < 10; i++) { if (i === 2) break; }
console.log(broke);

let acc = "";
for (let a = 0; a < 2; a++) {
  for (let b = 0; (acc += a + "" + b + ";"), b < 2; b++) {}
}
console.log(acc);
