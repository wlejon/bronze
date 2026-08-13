// Object rest into property references: `({ ...o.rest } = src)` and `({ ...o[k] } = src)`.

const o = {};
({ ...o.rest } = { a: 1, b: 2 });
console.log(o.rest.a, o.rest.b);

// With excluded property keys:
({ x: o.x, ...o.tail } = { x: 10, y: 20, z: 30 });
console.log(o.x, o.tail.y, o.tail.z, o.tail.x === undefined);

// Computed index target with side effect:
const target = {};
let counter = 0;
function getKey() {
    counter = counter + 1;
    return "k" + counter;
}
({ ...target[getKey()] } = { p: 100, q: 200 });
console.log(counter, target.k1.p, target.k1.q);

// Order of evaluation:
// Target reference is evaluated BEFORE rest object is copied.
const trace = [];
const receiver = {};
function getTarget() {
    trace.push("target");
    return receiver;
}
const src = {
    get a() { trace.push("get_a"); return 1; },
    get b() { trace.push("get_b"); return 2; }
};
({ ...getTarget().collected } = src);
console.log(trace.join(","));
console.log(receiver.collected.a, receiver.collected.b);
