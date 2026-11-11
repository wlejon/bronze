const log = [];
const o = (n) => ({ toString() { log.push('s' + n); return 'S' + n; }, valueOf() { log.push('v' + n); return n; } });
function f(x) { log.push('f' + x); return x; }
console.log(`${1}${2}`, `${o(1)}`, `a${o(2)}b${f(3) ? o(4) : 0}c${f(5)}`, `${''}`, ``, `x`, `${f(6)}${f(7)}${f(8)}`, log.join());
let big = '';
for (let i = 0; i < 2000; i++) big = `${big}${i % 10}-${i > 1000 ? 'é' : 'e'}\u{1F600}`.slice(-40);
console.log(big, `${null}:${undefined}:${1.5}:${true}:${[1, 2]}:${Symbol.iterator.description}`);
try { console.log(`${Symbol()}${f(9)}x`); } catch (e) { console.log(e.constructor.name, log.at(-1)); }
function* g() { return `${yield 1}-${yield 2}-${3}`; }
const it = g(); it.next(); it.next('a'); console.log(it.next('b').value);
