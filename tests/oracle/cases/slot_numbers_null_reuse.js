// A pool of records whose Number fields are nulled when a frame leaves them
// unused and set to Numbers again when reused (three.js's render list): the
// key is not given a double slot (opt/speculate/number_slots.cpp), and every
// record reaches the sort comparator, trained with all-Number records, with
// the one shape. Late rounds null fields of records still in use.
function cmp(a, b) {
    if (a.group !== b.group) return a.group - b.group;
    if (a.z !== b.z) return a.z - b.z;
    return a.id - b.id;
}
const pool = [];
let used = 0;
function next(id, group, z) {
    let r = pool[used];
    if (r === undefined) { r = { id: id, group: group, z: z, obj: null }; pool[used] = r; }
    else { r.id = id; r.group = group; r.z = z; }
    used++;
    return r;
}
function finish() {
    for (let i = used; i < pool.length; i++) { if (pool[i].id === null) break; pool[i].id = null; pool[i].obj = null; }
}
const out = [];
for (let f = 0; f < 600; f++) {
    used = 0;
    const list = [];
    const n = 50 + (f % 7) * 10;
    for (let i = 0; i < n; i++) list.push(next(i * 3 % n, i % 3, Math.sin(i * f) * 10));
    finish();
    if (f >= 590) { list[f % n].z = null; list[(f + 1) % n].id = "s"; }
    list.sort(cmp);
    let s = 0;
    for (let i = 0; i < list.length; i += 7) s += String(list[i].id).length + (list[i].z === null ? 1 : 0);
    if (f % 100 === 0 || f >= 590) out.push(f + ":" + s + ":" + list[0].id + ":" + list[list.length - 1].id);
}
console.log(out.join("\n"));
