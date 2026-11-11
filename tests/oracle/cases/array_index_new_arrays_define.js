// Every builtin that makes a NEW array fills it with CreateDataProperty
// (7.3.6), and an array literal's elements and a template's strings array
// are defined the same way (13.2.4.1, 13.2.8.4): none of them may run a
// setter Object.prototype holds at the same index, and each result keeps its
// elements as its own. Only a [[Set]] (the last two lines) runs it.
// A setter on Object.prototype[0..2] that stores nothing: every builtin that
// makes a NEW array defines its elements (CreateDataPropertyOrThrow), so none
// of them may run it, and the results keep their elements.
let log = '';
for (const i of [0, 1, 2]) {
}
let out = '';
function show(label, f) {
  log = '';
  let r;
  try { r = f(); } catch (e) { r = 'threw ' + e.constructor.name; }
  let s;
  if (r && typeof r === 'object') {
    s = '';
    for (let i = 0; i < r.length; i++) s += (Object.hasOwn(r, i) ? String(r[i]) : '<hole>') + ' ';
  } else s = String(r);
  out += label + ': ' + s + '| ' + log + '\n';
}
const src = [5, 6, 7];
show('literal', () => [1, 2, 3]);
show('map', () => src.map((x) => x * 2));
show('filter', () => src.filter(() => true));
show('slice', () => src.slice());
show('concat', () => src.concat([8]));
show('from', () => Array.from(src));
show('from iter', () => Array.from(new Set(src)));
show('of', () => Array.of(1, 2, 3));
show('split', () => 'a,b,c'.split(','));
show('keys', () => Object.keys({ a: 1, b: 2, c: 3 }));
show('entries', () => Object.entries({ a: 1 })[0]);
show('values', () => Object.values({ a: 1, b: 2 }));
show('match', () => 'abc'.match(/(a)(b)/));
show('matchAll', () => [...'aXbX'.matchAll(/X/g)].map((m) => m.index));
show('splice ret', () => [1, 2, 3, 4].splice(0, 3));
show('flat', () => [[1], [2, 3]].flat());
show('flatMap', () => [1, 2].flatMap((x) => [x, x]));
show('spread', () => [...src]);
show('rest', () => { const [a, ...rest] = [0, 1, 2, 3]; return rest; });
show('args rest', () => ((...xs) => xs)(1, 2, 3));
show('arguments', () => (function () { return arguments; })(1, 2, 3));
show('toSorted', () => src.toSorted());
show('toReversed', () => src.toReversed());
show('with', () => src.with(1, 9));
show('toSpliced', () => src.toSpliced(0, 0, 4));
show('JSON.parse', () => JSON.parse('[1,2,3]'));
show('fill new', () => new Array(3).fill(0));
show('array ctor', () => new Array(1, 2, 3));
show('array ctor n', () => Array(3));
show('getOwnPropertyNames', () => Object.getOwnPropertyNames({ x: 1, y: 2, z: 3 }));
show('Reflect.ownKeys', () => Reflect.ownKeys({ x: 1, y: 2 }));
show('str iter', () => [...'abc']);
show('typed to array', () => Array.from(new Uint8Array([1, 2, 3])));
show('map entries', () => [...new Map([[1, 2]])][0]);
show('groupBy', () => Object.groupBy([1, 2, 3], () => 'k').k);
show('promise all', () => 'skip');
show('template raw', () => ((s) => s.raw)`a${1}b${2}c`);
show('regexp exec', () => /(x)(y)/.exec('xy'));
show('sort holes', () => [3, , 1].sort());
show('copy', () => { const a = [0, , 2]; return a.copyWithin(1, 0, 1); });
show('push new', () => { const a = []; a.push(1, 2, 3); return a; });
show('assign idx', () => { const a = []; a[0] = 1; a[1] = 2; return a; });
console.log(out);
for (const i of [0, 1, 2]) delete Object.prototype[i];
