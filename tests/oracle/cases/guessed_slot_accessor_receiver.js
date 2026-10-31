// A static-slot claim on a GUESSED receiver class (a parameter whose naming
// callers agree, a key one class subtree owns) is guarded, and a receiver of
// any other kind takes the generic get / set: an object with an accessor for
// the key included. The IL passes that treat a proven claim's key as a plain
// data property (a store then a read gives the stored value, a read runs no
// getter, a read can be hoisted or reused) must not do so for these
// (il::provenStaticSlot). Each function here stores and reads, or reads
// twice, through such a site and is handed a receiver whose key is an
// accessor.

class Node {
    constructor(name) { this.name = name; this.payload = 0; this.link = null; }
    setAndRead(v) { this.link.payload = v; return this.link.payload; }
    readTwice() { const a = this.link.payload; const b = this.link.payload; return a + '/' + b; }
}
class Holder {
    constructor() { this.q = 1; }
    put(n, v) { n.payload = v; return n.payload; }
    twice(n) { return n.payload + '|' + n.payload; }
    loop(n, k) { let s = ''; for (let i = 0; i < k; i++) s += n.payload; return s; }
    nonNull(n) { n.payload = 1; return n.payload === null ? 'null' : typeof n.payload; }
}

const a = new Node('a');
const h = new Holder();
const log = [];
let reads = 0;
const acc = {
    get payload() { reads++; return reads % 2 ? 'G' : null; },
    set payload(v) { log.push(v); },
};
const out = [];
for (let i = 0; i < 4; i++) {
    a.link = i % 2 ? acc : new Node('n');
    out.push(a.setAndRead(i));
    out.push(a.readTwice());
    out.push(h.put(i % 2 ? acc : new Node('m'), 'p' + i));
}
console.log(out.join(','), log.join(','), reads);

const pool = [new Node('x'), acc];
const out2 = [];
for (let i = 0; i < 4; i++) {
    out2.push(h.put(new Node('k'), 'k' + i));
    out2.push(h.put(pool[i % 2], 'd' + i));
    out2.push(h.twice(new Node('t')));
    out2.push(h.twice(pool[i % 2]));
    out2.push(h.loop(new Node('l'), 3));
    out2.push(h.loop(pool[i % 2], 3));
    out2.push(h.nonNull(new Node('z')));
    out2.push(h.nonNull(pool[i % 2]));
}
console.log(out2.join(','), log.join(','), reads);
