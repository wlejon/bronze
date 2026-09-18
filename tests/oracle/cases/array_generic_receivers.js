// Generic Array.prototype methods called on array-like receivers (ECMA-262 23.1.3)

function serializeObj(obj) {
    const keys = Object.keys(obj).sort((a, b) => {
        const na = Number(a);
        const nb = Number(b);
        if (!isNaN(na) && !isNaN(nb)) return na - nb;
        if (a === 'length') return 1;
        if (b === 'length') return -1;
        return a.localeCompare(b);
    });
    return keys.map(k => `${k}:${JSON.stringify(obj[k])}`).join(', ');
}

// 1. Mutators
console.log('--- mutators ---');
{
    // push
    const obj = { length: 2, 0: 'a', 1: 'b' };
    const res = Array.prototype.push.call(obj, 'c', 'd');
    console.log('push:', res, serializeObj(obj));
}
{
    // pop
    const obj = { length: 3, 0: 'a', 1: 'b', 2: 'c' };
    const res = Array.prototype.pop.call(obj);
    console.log('pop:', res, serializeObj(obj));
}
{
    // shift
    const obj = { length: 3, 0: 'a', 1: 'b', 2: 'c' };
    const res = Array.prototype.shift.call(obj);
    console.log('shift:', res, serializeObj(obj));
}
{
    // unshift
    const obj = { length: 2, 0: 'b', 1: 'c' };
    const res = Array.prototype.unshift.call(obj, 'a');
    console.log('unshift:', res, serializeObj(obj));
}
{
    // reverse
    const obj = { length: 4, 0: 'a', 1: 'b', 2: 'c', 3: 'd' };
    const res = Array.prototype.reverse.call(obj);
    console.log('reverse:', res === obj, serializeObj(obj));
}
{
    // fill
    const obj = { length: 3, 0: 'a', 1: 'b', 2: 'c' };
    const res = Array.prototype.fill.call(obj, 'z', 1, 3);
    console.log('fill:', res === obj, serializeObj(obj));
}
{
    // copyWithin
    const obj = { length: 5, 0: 1, 1: 2, 2: 3, 3: 4, 4: 5 };
    const res = Array.prototype.copyWithin.call(obj, 0, 3);
    console.log('copyWithin:', res === obj, serializeObj(obj));
}
{
    // splice
    const obj = { length: 4, 0: 'a', 1: 'b', 2: 'c', 3: 'd' };
    const removed = Array.prototype.splice.call(obj, 1, 2, 'x', 'y', 'z');
    console.log('splice:', Array.isArray(removed), removed.join(','), serializeObj(obj));
}
{
    // sort
    const obj = { length: 4, 0: 30, 1: 10, 2: 40, 3: 20 };
    const res = Array.prototype.sort.call(obj, (a, b) => a - b);
    console.log('sort:', res === obj, serializeObj(obj));
}

// 2. Search
console.log('--- search ---');
{
    const obj = { length: 4, 0: 'a', 1: 'b', 2: 'a', 3: 'c' };
    console.log('indexOf:', Array.prototype.indexOf.call(obj, 'a', 1));
    console.log('lastIndexOf:', Array.prototype.lastIndexOf.call(obj, 'a'));
    console.log('includes:', Array.prototype.includes.call(obj, 'b'));
    console.log('at:', Array.prototype.at.call(obj, -1));
    console.log('find:', Array.prototype.find.call(obj, x => x === 'c'));
    console.log('findIndex:', Array.prototype.findIndex.call(obj, x => x === 'c'));
    console.log('findLast:', Array.prototype.findLast.call(obj, x => x === 'a'));
    console.log('findLastIndex:', Array.prototype.findLastIndex.call(obj, x => x === 'a'));
}

// 3. Iteration
console.log('--- iteration ---');
{
    const obj = { length: 3, 0: 1, 1: 2, 2: 3 };
    const visited = [];
    Array.prototype.forEach.call(obj, (v, i) => visited.push(`${i}:${v}`));
    console.log('forEach:', visited.join(','));

    const mapped = Array.prototype.map.call(obj, x => x * 10);
    console.log('map:', Array.isArray(mapped), mapped.join(','));

    const filtered = Array.prototype.filter.call(obj, x => x % 2 !== 0);
    console.log('filter:', Array.isArray(filtered), filtered.join(','));

    console.log('some:', Array.prototype.some.call(obj, x => x === 2));
    console.log('every:', Array.prototype.every.call(obj, x => x > 0));
    console.log('reduce:', Array.prototype.reduce.call(obj, (acc, x) => acc + x, 0));
    console.log('reduceRight:', Array.prototype.reduceRight.call(obj, (acc, x) => acc - x, 100));
}

// 4. Transform
console.log('--- transform ---');
{
    const obj = { length: 3, 0: 'a', 1: 'b', 2: 'c' };
    const sliced = Array.prototype.slice.call(obj, 1, 3);
    console.log('slice:', Array.isArray(sliced), sliced.join(','));

    const concat1 = Array.prototype.concat.call(obj, ['d', 'e']);
    console.log('concat default:', concat1.length, concat1[0] === obj, concat1.slice(1).join(','));

    const spreadable = { [Symbol.isConcatSpreadable]: true, length: 2, 0: 'x', 1: 'y' };
    const concat2 = Array.prototype.concat.call(spreadable, ['z']);
    console.log('concat spreadable:', concat2.join(','));

    console.log('join:', Array.prototype.join.call(obj, ':'));

    const nested = { length: 2, 0: [1, 2], 1: [3, 4] };
    const fl = Array.prototype.flat.call(nested);
    console.log('flat:', fl.join(','));

    const fm = Array.prototype.flatMap.call(obj, x => [x, x.toUpperCase()]);
    console.log('flatMap:', fm.join(','));

    const toSort = { length: 3, 0: 30, 1: 10, 2: 20 };
    const sorted = Array.prototype.toSorted.call(toSort, (a, b) => a - b);
    console.log('toSorted:', sorted.join(','), serializeObj(toSort));

    const rev = Array.prototype.toReversed.call(obj);
    console.log('toReversed:', rev.join(','), serializeObj(obj));

    const sp = Array.prototype.toSpliced.call(obj, 1, 1, 'x', 'y');
    console.log('toSpliced:', sp.join(','), serializeObj(obj));

    const w = Array.prototype.with.call(obj, 1, 'replaced');
    console.log('with:', w.join(','), serializeObj(obj));
}

// 5. Iterators
console.log('--- iterators ---');
{
    const obj = { length: 3, 0: 'a', 1: 'b', 2: 'c' };
    console.log('keys:', [...Array.prototype.keys.call(obj)].join(','));
    console.log('values:', [...Array.prototype.values.call(obj)].join(','));
    console.log('entries:', [...Array.prototype.entries.call(obj)].map(([k, v]) => `${k}=${v}`).join(','));
}

// 6. Strings as array-likes
console.log('--- strings ---');
{
    const s = 'hello';
    console.log('string join:', Array.prototype.join.call(s, '-'));
    console.log('string slice:', Array.prototype.slice.call(s, 1, 4).join(','));
    console.log('string map:', Array.prototype.map.call(s, c => c.toUpperCase()).join(''));
    console.log('string indexOf:', Array.prototype.indexOf.call(s, 'l', 3));
    console.log('string includes:', Array.prototype.includes.call(s, 'ell'));
    console.log('string at:', Array.prototype.at.call(s, -1));
    console.log('string reduce:', Array.prototype.reduce.call(s, (acc, c) => c + acc, ''));
}

// 7. Holes on array-likes
console.log('--- holes ---');
{
    const sparse = { length: 4, 0: 'first', 3: 'last' };
    console.log('sparse join:', Array.prototype.join.call(sparse, ','));
    console.log('sparse includes hole:', Array.prototype.includes.call(sparse, undefined));
    console.log('sparse indexOf hole:', Array.prototype.indexOf.call(sparse, undefined));

    const mapped = Array.prototype.map.call(sparse, x => x + '!');
    console.log('sparse map:', 1 in mapped, mapped[0], mapped[3]);

    const filtered = Array.prototype.filter.call(sparse, () => true);
    console.log('sparse filter:', filtered.length, filtered.join(','));

    const sortedSparse = { length: 4, 0: 'b', 2: 'a' };
    Array.prototype.sort.call(sortedSparse);
    console.log('sparse sort:', serializeObj(sortedSparse));
}

// 8. Null and undefined receivers
console.log('--- null and undefined ---');
function tryCall(fn, thisVal) {
    try {
        fn.call(thisVal);
        return 'no-throw';
    } catch (e) {
        return e.name;
    }
}
console.log('slice(null):', tryCall(Array.prototype.slice, null));
console.log('push(undefined):', tryCall(Array.prototype.push, undefined));
console.log('map(null):', tryCall(Array.prototype.map, null));
console.log('sort(undefined):', tryCall(Array.prototype.sort, undefined));
