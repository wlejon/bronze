// A RUN-ARM GROUP IN A BLOCK THE SLOW COPY WAS EMITTED IN FRONT OF.
//
// A guarded numeric region (src/lower/guard_region.h) lays its SLOW copy out as
// a low-numbered block that only the bottom of the fast copy branches to, and
// the backend emits blocks in index order — so the slow copy's own reloads run
// between the block a live value's register was written in and the block that
// wants to hand that register to a run-arm group's join (llvm_run_arms.h). The
// register left behind dominates neither arm.
//
// `p4` is the smallest shape that gets there: two consecutive element reads
// feeding four stores through one guarded region, with `out` live across the
// whole thing. Everything below it is the same question asked where a wrong
// answer is a moved object rather than a rejected module:
//
//   `pick`    puts a DIAMOND in front of the group, so the value the join has
//             to hand forward arrives as a block parameter rather than as a
//             register some single predecessor left.
//   `churn`   puts a LOOP between the group and the reads of what it produced,
//             which is the case where the results have to survive an allocation
//             the fast arm wrote no root slot for.
//   `twoRuns` puts a second group in a LATER BLOCK with the first group's
//             results live across it: those results are handed on by the second
//             join's phi and their own slots were never written, so a read sent
//             to a slot instead of to the phi is a read of a word nothing has
//             stored to.
//   `tag`     carries a STRING across the group, so what the join forwards is
//             not a number the representation plan could keep out of the heap.
//
// Every one of them is called with a receiver the proof refuses — a plain
// object — and with a string operand that makes the guarded region bail into
// the slow copy that was emitted first.

function show(a, n) {
    let s = '';
    for (let i = 0; i < n; i++) {
        if (i) s += ',';
        s += String(a[i]);
    }
    return s;
}

function wrap(x) { return { n: x }; }

function p4(out, src) {
    const a = src[0], b = src[1];
    out[0] = a * b;
    out[1] = a + b;
    out[2] = a - b;
    out[3] = b - a;
    return out;
}

function q4(out, src) {
    const a = src[0], b = src[1], c = src[2], d = src[3];
    out[0] = a + b;
    out[1] = b + c;
    out[2] = c + d;
    out[3] = d + a;
    return show(out, 4);
}

function pick(out, src, flag) {
    let base;
    if (flag) { base = wrap(1); } else { base = wrap(2); }
    const a = src[0], b = src[1];
    out[0] = a + base.n;
    out[1] = b + base.n;
    return String(base.n) + ':' + show(out, 2);
}

function churn(src, n) {
    const a = src[0], b = src[1], c = src[2];
    const junk = [];
    for (let i = 0; i < n; i++) junk.push({ i: i });
    return String(a) + '/' + String(b) + '/' + String(c) + '/' + String(junk.length);
}

function twoRuns(a, b, n) {
    const a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
    const junk = [];
    for (let i = 0; i < n; i++) junk.push({ i: i });
    const b0 = b[0], b1 = b[1], b2 = b[2], b3 = b[3];
    return String(a0 + b0) + ',' + String(a1 + b1) + ',' +
           String(a2 + b2) + ',' + String(a3 + b3);
}

function tag(out, src, label) {
    const a = src[0], b = src[1];
    out[0] = a * b;
    return label + ':' + show(out, 1);
}

// 1. The p4 shape: dense, then a string operand that bails the region into the
//    slow copy, then a receiver that is no Array at all on either side.
console.log(show(p4([0, 0, 0, 0], [3, 5]), 4));
console.log(show(p4([0, 0, 0, 0], ['3', 5]), 4));
console.log(show(p4({}, [3, 5]), 4));
console.log(show(p4([0, 0, 0, 0], { 0: 3, 1: 5 }), 4));
console.log(show(p4([0, 0, 0, 0], [3, 5]), 4));

// 2. Four reads and four stores, with the bail landing at different members.
console.log(q4([0, 0, 0, 0], [1, 2, 3, 4]));
console.log(q4([0, 0, 0, 0], [1, 2, 'x', 4]));
console.log(q4([0, 0, 0, 0], ['1', 2, 3, 4]));
console.log(q4([0, 0, 0, 0], { 0: 1, 1: 2, 2: 3, 3: 4 }));
console.log(q4([0, 0, 0, 0], [1, 2, 3, 4]));

// 3. A diamond in front of the group.
console.log(pick([0, 0], [10, 20], true));
console.log(pick([0, 0], [10, 20], false));
console.log(pick([0, 0], { 0: 10, 1: 20 }, true));
console.log(pick([0, 0], ['10', 20], false));

// 4. A loop between the group and the reads of what it produced.
console.log(churn([7, 8, 9], 6));
console.log(churn({ 0: 7, 1: 8, 2: 9 }, 6));
console.log(churn([7, 8, 'z'], 4));
console.log(churn([7, 8, 9], 0));

// 5. A second group in a later block, carrying the first group's results.
console.log(twoRuns([1, 2, 3, 4], [10, 20, 30, 40], 5));
console.log(twoRuns({ 0: 1, 1: 2, 2: 3, 3: 4 }, [10, 20, 30, 40], 5));
console.log(twoRuns([1, 2, 3, 4], { 0: 10, 1: 20, 2: 30, 3: 40 }, 5));
console.log(twoRuns(['1', 2, 3, 4], [10, 20, 30, 40], 3));
console.log(twoRuns([1, 2, 3, 4], [10, 20, 30, 40], 0));

// 6. A string live across the group.
console.log(tag([0], [6, 7], 'lbl'));
console.log(tag([0], { 0: 6, 1: 7 }, 'lbl'));
console.log(tag([0], ['6', 7], 'str'));
