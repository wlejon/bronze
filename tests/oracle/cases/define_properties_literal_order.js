// WHEN the parts of `Object.defineProperties(target, { ... })` run: ECMA-262
// 13.3.6.1 EvaluateCall, then 20.1.2.3.
//
// A call evaluates its arguments completely before the function runs, and the
// descriptor map is an argument — so every `value` expression in it has already
// run by the time step 1 asks whether the target is an Object. A target that is
// not one therefore throws AFTER those side effects, never instead of them.
// That is the ordering fact a lowering which never builds the map has to
// preserve: the target expression first (it is the earlier argument), then each
// descriptor's `value` in the order the literal wrote it, then the check.
//
// 20.1.2.3.1 then decodes every descriptor (step 4) and only afterwards defines
// them (step 5). The split is invisible for descriptors written as literals of
// plain values — there is no getter to run and no decode that can fail — which
// is what `cases/blocked/descriptor_define_all_or_nothing` is for and this case
// deliberately stays clear of.

function attempt(fn) {
    try {
        fn();
        return 'ok';
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

const log = [];
function note(tag, v) {
    log.push(tag);
    return v;
}

// 1. Target first, then the `value` expressions in source order — including the
// one whose descriptor writes `enumerable` before `value`, because it is the
// LITERAL's order that decides when the expression runs, not the field's
// position in 6.2.6.5. The call answers the target itself (step 3).
const t = {};
const r = Object.defineProperties(note('target', t), {
    a: { value: note('a', 1), enumerable: true },
    b: { enumerable: true, value: note('b', 2) },
    c: { value: note('c', 3), enumerable: true },
});
console.log('1', log.join(','), r === t, JSON.stringify(t));

// 2. A primitive target: the arguments still ran, both of them, and the
// TypeError is 20.1.2.3 step 1's.
log.length = 0;
console.log('2', attempt(() => Object.defineProperties(note('t2', 5), {
    a: { value: note('a2', 1) },
})), log.join(','));

// 3. `null` has no side effect of its own, so what this shows is the other
// half: the descriptor's `value` ran before the target was rejected.
log.length = 0;
console.log('3', attempt(() => Object.defineProperties(null, {
    a: { value: note('a3', 1) },
})), log.join(','));

// 4. And an ordinary target keeps the same order when the target expression is
// itself a call that allocates.
log.length = 0;
function fresh() {
    log.push('fresh');
    return {};
}
const f = Object.defineProperties(fresh(), {
    p: { value: note('p', 'P'), enumerable: true },
    q: { value: note('q', 'Q'), enumerable: true },
});
console.log('4', log.join(','), JSON.stringify(f));
