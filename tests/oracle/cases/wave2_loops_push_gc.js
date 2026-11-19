function fillTypedOffset(el, out) {
    let idx = el.start;
    const size = el.size;
    let sum = 0;
    for (let i = 0; i < size; i++) {
        out[idx++] = i * 3 + el.bias;
        sum += out[idx - 1];
    }
    return sum + idx;
}

function pushParamMulti(nums, objs, n) {
    let totalLen = 0;
    for (let i = 0; i < n; i++) {
        totalLen += nums.push(i, i + 1);
        totalLen += nums.push(i + 2, i + 3, i + 4, i + 5);
        totalLen += nums.push(1, 2, 3, 4, 5, 6, 7, 8);
        objs.push(i, { k: i, v: 's' + (i & 3) }, i * 0.5, { m: [i, i + 1] });
    }
    return totalLen;
}

function pushLocalAndDemote(n) {
    const local = [1.5, 2.5];
    let ret = 0;
    for (let i = 0; i < n; i++) {
        ret += local.push(i, i * 2, i * 3);
    }
    ret += local.push(10, { tag: 'demoted', arr: [1, 2, 3] }, 30, 'tail');
    let sum = 0;
    for (let i = 0; i < local.length; i++) {
        const v = local[i];
        sum += typeof v === 'number' ? v : (v.arr ? v.arr[2] : v.length);
    }
    return ret + ':' + local.length + ':' + sum.toFixed(1);
}

class VecMixed {
    constructor(x, tag, y, next, z, w, extra) {
        this.x = x;
        this.tag = tag;
        this.y = y;
        this.next = next;
        this.z = z;
        this.w = w;
        this.extra = extra;
    }
}

function exerciseGcMixedAndNumberBlocks() {
    const retainNums = [];
    const retainMixed = [];
    const retainObjs = [];
    const retainDates = [];
    let acc = 0;
    for (let i = 0; i < 6000; i++) {
        const na = [];
        na.push(i * 0.25, i + 1, i + 2, i + 3);
        const lit = [i, i + 0.5, i + 1.5, i + 2.5];
        const demote = [1, 2, 3, 4];
        if ((i & 3) === 0) {
            demote.push({ id: i });
        } else {
            demote[1] = { id: i * 2 };
        }
        const v = new VecMixed(
            i * 1.25,
            'v' + (i & 7),
            i * 2.5,
            { nested: i + 7 },
            i * 3.75,
            i * 0.5,
            i + 9
        );
        acc += na[3] + lit[1] + v.x + v.y + v.next.nested;
        if ((i & 63) === 0) {
            retainNums.push(na, lit);
            retainMixed.push(demote);
            retainObjs.push(v);
            const d = new Date(1700000000000 + i * 1000);
            d.numProp = i * 1.5;
            d.objProp = { yr: d.getUTCFullYear(), idx: i };
            retainDates.push(d);
        }
    }
    let check = 0;
    for (let i = 0; i < retainNums.length; i++) {
        check += retainNums[i][0] + retainNums[i][2];
    }
    for (let i = 0; i < retainMixed.length; i++) {
        const a = retainMixed[i];
        const last = a.length === 5 ? a[4].id : a[1].id;
        check += a[0] + last;
    }
    for (let i = 0; i < retainObjs.length; i++) {
        const v = retainObjs[i];
        check += v.x + v.y + v.z + v.w + v.extra + v.tag.length + v.next.nested;
    }
    for (let i = 0; i < retainDates.length; i++) {
        const d = retainDates[i];
        check += d.numProp + d.objProp.yr + d.objProp.idx;
    }
    return acc.toFixed(1) + ':' + check.toFixed(1) + ':' + retainObjs.length;
}

// 1. TypedArray loops + slot-loaded start index + byteOffset > 0 + deopt
const backing = new ArrayBuffer(128);
const view16 = new Uint16Array(backing, 8, 24);
const viewF32 = new Float32Array(backing, 16, 20);
let taSum = 0;
for (let r = 0; r < 120; r++) {
    taSum += fillTypedOffset({ start: 2, size: 10, bias: r & 7 }, view16);
    taSum += fillTypedOffset({ start: 1, size: 8, bias: r & 3 }, viewF32);
}
// 0-trip loop with undefined start index
taSum += fillTypedOffset({ start: undefined, size: 0, bias: 5 }, view16) || 0;
// Deopt case: fractional start index
const deoptView = new Float32Array(12);
const deoptRes = fillTypedOffset({ start: 1.5, size: 3, bias: 2 }, deoptView);
console.log('ta', taSum, view16[5], viewF32[4], deoptRes.toFixed(1), deoptView[1], deoptView[2]);

// 2. Multi-arg push on param arrays & local arrays
const pNums = [];
const pObjs = [100];
let pLenSum = 0;
for (let r = 0; r < 50; r++) {
    pLenSum += pushParamMulti(pNums, pObjs, 4);
}
console.log('pushParam', pNums.length, pNums[0], pNums[pNums.length - 1], pObjs.length, pObjs[2].k, pLenSum);
console.log('pushLocal', pushLocalAndDemote(30));

// 3. GC stress: NumberBlock, demoted ValueBlock, double_slots VecMixed, Date + extra props
console.log('gc', exerciseGcMixedAndNumberBlocks());
