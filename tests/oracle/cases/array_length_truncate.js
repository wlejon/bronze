// `arr.length = n` on ordinary, sealed, frozen and named-property arrays:
// truncation deletes, growth leaves holes, invalid lengths throw.
'use strict';
function show(label, a) {
  const keys = Object.keys(a).join(',');
  console.log(label, a.length, '[' + keys + ']', JSON.stringify(a));
}

const objs = [];
for (let i = 0; i < 6; i++) objs.push({ i });
const a = objs.slice();
a.length = 3;
show('truncate', a);
a.length = 5;
show('grow', a);
console.log('hole', 3 in a, a[3]);
a.length = 0;
show('clear', a);
a.push(7);
show('push after clear', a);
a.length = 1;
show('same', a);
a.length = 1.0;
show('same double', a);

for (const bad of [-1, 1.5, NaN, 4294967296, '2x']) {
  const b = [1, 2, 3];
  try { b.length = bad; console.log('set', String(bad), b.length); } catch (e) { console.log('bad', String(bad), e.constructor.name); }
}
const c = [1, 2, 3, 4];
c.length = '2';
show('string', c);
c.length = { valueOf() { return 1; } };
show('valueOf', c);

const named = [1, 2, 3];
named.tag = 'x';
named.length = 1;
show('named', named);
console.log(named.tag);

const sealed = Object.seal([1, 2, 3]);
try { sealed.length = 1; } catch (e) { console.log('sealed', e.constructor.name); }
show('sealed', sealed);
sealed.length = 3;
show('sealed same', sealed);

const frozen = Object.freeze([1, 2, 3]);
try { frozen.length = 0; } catch (e) { console.log('frozen', e.constructor.name); }
show('frozen', frozen);

const holey = [1, , 3, , 5];
holey.length = 2;
show('holey', holey);

let sum = 0;
const work = [];
for (let round = 0; round < 2000; round++) {
  for (let k = 0; k < (round % 7); k++) work.push(k + round);
  for (const v of work) sum += v;
  work.length = 0;
}
console.log('loop', sum, work.length);
