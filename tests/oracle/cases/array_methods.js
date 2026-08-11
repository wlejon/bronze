// Array.prototype. Reading any of these names is a named hard error today
// ("unsupported: Array.prototype.push is not implemented") rather than the
// `undefined` it used to read. Results are joined into strings on purpose:
// console.log of a container has no pinned format yet (docs/0009), and
// inventing one here would pin it in an .expected as a side effect.
const a = [3, 1, 2];
console.log(a.length);
a.push(4);
console.log(a.length);
console.log(a[3]);
console.log(a.pop());
console.log(a.length);
console.log(a.indexOf(1));
console.log(a.indexOf(99));
console.log(a.includes(2));
console.log(a.join("-"));
const b = [1, 2, 3];
console.log(b.map(function (x) { return x * 2; }).join(","));
console.log(b.filter(function (x) { return x > 1; }).join(","));
console.log(b.reduce(function (acc, x) { return acc + x; }, 0));
console.log(b.slice(1).join(","));
console.log(b.concat([4]).join(","));
console.log(b.reverse().join(","));
