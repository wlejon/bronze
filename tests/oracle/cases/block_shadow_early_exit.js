// A `break` or `continue` taken inside a block whose own `let`/`const`
// shadows a name the loop (or switch, or labelled block) carries must hand the
// target the OUTER binding. Every form below once passed the inner binding's
// value along the jump edge, so the outer `o` came out as 'i'.
let o = 0;
for (let t = 1; t < 4; t++) { o = t; { let o = 'i'; if (t === 2) continue; } }
console.log(o);
o = 0;
for (let t = 1; t < 4; t++) { o = t; { let o = 'i'; if (t === 2) break; } }
console.log(o);
o = 0;
while (true) { o = 5; { let o = 'i'; break; } }
console.log(o);
o = 0;
x: { o = 6; { let o = 'i'; break x; } }
console.log(o);
o = 0;
switch (1) { case 1: o = 7; { let o = 'i'; break; } }
console.log(o);
o = 0;
do { o = 8; { const o = 'i'; if (o) break; } } while (false);
console.log(o);
o = 0;
for (const t of [9, 10]) { o = t; { let o = 'i'; continue; } }
console.log(o);
o = 0;
for (const k in { a: 1 }) { o = k; { let o = 'i'; break; } }
console.log(o);
o = 0;
for (let i = 0; i < 3; i++) { { let i = 'inner'; if (o++ > 5) break; continue; } }
console.log(o);
