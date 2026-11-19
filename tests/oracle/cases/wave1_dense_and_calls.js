function denseWriteLoop(arr) {
    let sum = 0;
    for (let i = 0; i < arr.length; i++) {
        arr[i] = arr[i] * 2 + (i & 7);
        sum += arr[i];
    }
    return sum;
}

function transitionMidLoop(arr, flipAt) {
    let acc = 0;
    for (let i = 0; i < arr.length; i++) {
        const v = arr[i];
        acc += typeof v === 'number' ? v : v.length;
        if (i === flipAt) {
            arr[i] = 'str_' + i;
        } else {
            arr[i] = (typeof v === 'number' ? v : 1) + 1;
        }
    }
    let tail = 0;
    for (let i = 0; i < arr.length; i++) {
        const x = arr[i];
        tail += typeof x === 'number' ? x : x.length;
    }
    return acc * 100 + tail;
}

function addTwo(a, b) {
    return this.base + a * 3 + b;
}

function readZero() {
    return this.base + 7;
}

function exerciseCalls(arr, fn0, fn2) {
    let total = 0;
    const ctx = { base: 10 };
    for (let i = 0; i < 400; i++) {
        arr.push(i * 1.5);
        const top = arr.pop();
        total += top;
        ctx.base = i & 15;
        total += fn0.call(ctx);
        total += fn2.call(ctx, i & 7, (i + 3) & 7);
    }
    const custom = {
        call(o, x) { return o.base + x + 1000; }
    };
    total += custom.call(ctx, 5);
    return total;
}

function exerciseLiteralChurn() {
    const keep = [];
    let check = 0;
    for (let i = 0; i < 40000; i++) {
        const inner = { v: i + 1, tag: 't' + (i & 3) };
        const outer = { x: i, y: inner, z: [i & 7, (i + 1) & 7] };
        if ((i & 511) === 0) {
            keep.push(outer);
        }
        check = (check + outer.y.v + outer.z[1]) | 0;
    }
    let retained = 0;
    for (let i = 0; i < keep.length; i++) {
        retained += keep[i].y.v + keep[i].y.tag.length + keep[i].z[0];
    }
    return check + ':' + retained + ':' + keep.length;
}

function exerciseMathLeafs() {
    let s = 0;
    for (let i = -20; i <= 20; i++) {
        const x = i * 0.35;
        s += Math.round(x) + Math.min(x, -1.5) + Math.max(x, 1.5);
    }
    return s.toFixed(4);
}

const a = [1, 2, 3, 4, 5, 6, 7, 8];
let denseSum = 0;
for (let r = 0; r < 500; r++) {
    for (let i = 0; i < a.length; i++) a[i] = (i + 1) & 15;
    denseSum = denseWriteLoop(a);
}
console.log('dense', denseSum, a[0], a[7]);

let transOut = [];
for (let r = 0; r < 200; r++) {
    const b = [10, 20, 30, 40, 50];
    const res = transitionMidLoop(b, r === 199 ? 2 : -1);
    if (r === 198 || r === 199) transOut.push(res + ':' + typeof b[2] + ':' + b[2]);
}
const bCheck = [10, 20, 'mid', 40, 50];
console.log('trans', transOut.join('|'), transitionMidLoop(bCheck, 1), typeof bCheck[1], bCheck[2]);

console.log('calls', exerciseCalls([], readZero, addTwo));
console.log('churn', exerciseLiteralChurn());
console.log('math', exerciseMathLeafs());
