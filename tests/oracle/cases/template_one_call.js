// A template's pieces are all Strings once each substitution is converted, so
// the chain is one call that allocates the result at its length
// (il_string_join.cpp, bronze_concat_strings); `a + b` of two Latin-1 strings
// allocates without rooting them.
function g(a, b, c, d) { return `<${a}|${b}|${c}|${d}>`; }
const big = 'x'.repeat(5000);
console.log(g('é', 'ascii', '日本', 1.25), g(big, big, big, big).length, g('', '', '', ''), g(null, undefined, true, -0));
console.log(g(1n, [1, [2, 3]], {}, () => 1), g(NaN, Infinity, -1e21, 1e-7));

// Each substitution is converted where it is evaluated, before the next one.
const log = [];
const o = { n: 0, toString() { log.push('s' + this.n); return 's' + this.n++; }, valueOf() { log.push('v'); return 7; } };
function side(tag) { log.push(tag); return tag; }
console.log(`${o}${side('a')}${o}${side('b')}${o}`, log.join(','));

// A Symbol refuses at its own place: what follows is not evaluated.
try { console.log(`${side('before')}${Symbol('q')}${side('after')}`); } catch (e) { console.log(e.constructor.name, log.slice(-1)[0]); }

// A substitution that throws leaves nothing half built.
function boom() { throw new RangeError('boom'); }
try { console.log(`a${1}b${boom()}c`); } catch (e) { console.log(e.message); }

// Many pieces, more than the join takes in one call, and a nested template.
const p = ['a', 'b', 'c'];
console.log(`${p[0]}${p[1]}${p[2]}${p[0]}${p[1]}${p[2]}${p[0]}${p[1]}${p[2]}${`[${p[0]}-${p[1]}-${p[2]}]`}${p.length}`);

// A long run allocates through collections: the pieces stay what they were.
let s = '';
let total = 0;
for (let i = 0; i < 30000; i++) {
    s = `${s.length % 7}${i}${big.slice(0, i % 50)}é`.slice(0, 40);
    total += s.length;
}
console.log(s, total);

// `+` of two strings: Latin-1 with Latin-1, with UTF-16, with empty, one char.
const names = ['normal', 'bold', '', 'é', 'x', '日本'];
let len = 0, last = '';
for (let i = 0; i < 60000; i++) { last = names[i % 6] + names[(i + 3) % 6]; len += last.length; }
console.log(len, last, 'a' + '', '' + 'é', 'é' + 'b', ('x'.repeat(70000) + 'y'.repeat(70000)).length);
