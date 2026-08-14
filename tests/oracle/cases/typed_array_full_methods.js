const ofArr = Int32Array.of(10, 20, 30);
console.log(ofArr.join(","));

const fromArr = Float64Array.from([1.5, 2.5, 3.5]);
console.log(fromArr.join(","));

const fromMapped = Uint8Array.from([1, 2, 3], function (x) { return x * 10; });
console.log(fromMapped.join(","));

const ta = new Uint8Array([50, 20, 10, 40, 30]);
console.log(ta.at(0));
console.log(ta.at(-1));
console.log(ta.at(10));

console.log(ta.indexOf(40));
console.log(ta.lastIndexOf(20));
console.log(ta.includes(10));

console.log(ta.find(function (x) { return x < 25; }));
console.log(ta.findIndex(function (x) { return x < 25; }));
console.log(ta.findLast(function (x) { return x < 25; }));
console.log(ta.findLastIndex(function (x) { return x < 25; }));

console.log(ta.every(function (x) { return x > 0; }));
console.log(ta.some(function (x) { return x === 100; }));

console.log(ta.filter(function (x) { return x >= 30; }).join(","));
console.log(ta.map(function (x) { return x + 1; }).join(","));
console.log(ta.reduce(function (acc, x) { return acc + x; }, 0));
console.log(ta.reduceRight(function (acc, x) { return acc - x; }, 150));

console.log(ta.toSorted().join(","));
console.log(ta.toReversed().join(","));
console.log(ta.with(2, 99).join(","));

ta.sort();
console.log(ta.join(","));

ta.reverse();
console.log(ta.join(","));

let keys = "";
for (const k of ta.keys()) {
    keys += (keys.length > 0 ? "," : "") + k;
}
console.log(keys);

let vals = "";
for (const v of ta.values()) {
    vals += (vals.length > 0 ? "," : "") + v;
}
console.log(vals);

let entries = "";
for (const [k, v] of ta.entries()) {
    entries += (entries.length > 0 ? ";" : "") + k + ":" + v;
}
console.log(entries);
