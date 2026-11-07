// A tiny leaf (MathUtils.clamp's shape) called from functions with no loop:
// the call is copied in, so its argument conversions, their order and the
// values it returns must be the call's. The branchy caller unboxes the same
// value on one arm and calls on another, so a conversion made on one arm is
// never what the call on the other reads.
function clamp(value, min, max) { return Math.max(min, Math.min(max, value)); }
function twice(x) { return x + x; }
function neg(x) { return -x; }

const log = [];
function num(name, v) { return { valueOf() { log.push(name); return v; } }; }

function asinClamped(te) { return Math.asin(clamp(te[2], -1, 1)); }

function pick(order, m11, m12, m13) {
    let y, x;
    switch (order) {
        case 'A':
            y = Math.asin(clamp(m13, -1, 1));
            x = m11 * 2;
            break;
        case 'B':
            x = m12 + m13;
            y = -Math.asin(clamp(m13, -1, 1));
            break;
        case 'C':
            y = m13 * 1;
            x = clamp(m11, 0, y);
            break;
        default:
            y = clamp(m12, m11, m13);
            x = twice(y);
    }
    return [x, y];
}

// A caller with a loop: one arm unboxes v, the other passes it to a call.
function sumLoop(vs) {
    let s = 0;
    for (let i = 0; i < vs.length; i++) {
        const v = vs[i];
        if (i % 2) s += v * 0.5;
        else s += clamp(v, -2, 2);
    }
    return s;
}

function show(v) { return Object.is(v, -0) ? '-0' : String(v); }
const out = [];
for (const o of ['A', 'B', 'C', 'D']) {
    out.push(o + ' ' + pick(o, 0.25, 0.5, 2).map(show).join(','));
    out.push(o + ' ' + pick(o, -3, 4, -0.5).map(show).join(','));
    out.push(o + ' ' + pick(o, 1, NaN, 0.75).map(show).join(','));
}
out.push(show(asinClamped([0, 0, 3])), show(asinClamped([0, 0, -0])), show(asinClamped([0, 0, '0.5'])));
out.push(show(clamp(-0, 0, 1)), show(clamp(0, -0, 1)), show(clamp(5, 1, NaN)));
out.push(show(clamp(num('v', 7), num('lo', 0), num('hi', 3))), log.join(' '));
log.length = 0;
out.push(show(pick('D', num('a', 1), num('b', 9), num('c', 4))[1]), log.join(' '));
out.push(show(twice('ab')), show(twice(2.5)), show(neg(0)), show(neg('3')));
out.push(show(sumLoop([5, 3, -7, '4', 1.5, -0])));
console.log(out.join('\n'));
