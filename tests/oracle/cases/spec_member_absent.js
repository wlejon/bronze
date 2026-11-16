// Reads of a key that is on neither the receiver nor its prototype chain,
// over three receiver shapes (absent-member cells: the read is `undefined`
// behind a shape test), and a method found on the prototype chain of
// receivers of seven shapes (a group of member cells wider than a shape
// chain, whose miss is done generically in S). Partway, prototypes change in
// ways that leave both answers standing (a method added to one, another
// method overwritten on the root), so every cell goes stale and is filled
// again. A harvest samples the whole run, so a change that alters either
// answer at these sites would also keep them from being speculated; those
// are read at sites of their own at the end.
class Root { constructor(i) { this.i = i; } step() { return this.i + 1; } other() { return 0; } }
class K1 extends Root { constructor(i) { super(i); this.a = 1; } }
class K2 extends Root { constructor(i) { super(i); this.b = 2; } }
class K3 extends Root { constructor(i) { super(i); this.c = 3; } }
class K4 extends Root { constructor(i) { super(i); this.d = 4; } }
class K5 extends Root { constructor(i) { super(i); this.e = 5; } }
class K6 extends K5 { constructor(i) { super(i); this.f = 6; } }
function probe(o) {
  // `isGroup` is on nothing: absent.
  return o.isGroup === undefined ? 1 : 100;
}
function walk(list) {
  let s = 0;
  for (let k = 0; k < list.length; k++) {
    const o = list[k];
    s += o.step() + (o.i % 7 < 3 ? probe(o) : 0);
  }
  return s;
}
const kinds = [Root, K1, K2, K3, K4, K5, K6];
const list = [];
for (let i = 0; i < 28; i++) list.push(new kinds[i % kinds.length](i));
let total = 0;
for (let r = 0; r < 3000; r++) total += walk(list);
console.log('warm', total);

const out = [];
K4.prototype.extra = function () { return 1; };
out.push(walk(list));
Root.prototype.other = function () { return 2; };
out.push(walk(list));
K2.prototype.isGroupish = 3;
out.push(walk(list));
console.log(out.join());
for (let r = 0; r < 3000; r++) total += walk(list);
console.log('end', total);

// What would change the answers, at sites of their own.
const late = (o) => [o.isGroup, o.step()];
const swapped = new K1(14);
Object.setPrototypeOf(swapped, { step() { return 7; }, isGroup: 1 });
const shadow = new K5(5);
shadow.step = () => -1;
Root.prototype.isGroup = 'root';
console.log(late(swapped).join(), late(shadow).join(), late(list[1]).join());
