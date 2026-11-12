// Method calls whose receiver is a RegExp, a number or a string, repeated at
// one site so the site's cache answers them: the builtin reached must be the
// one a by-name lookup finds, for every receiver the site meets.
const out = [];
function log(...a) { out.push(a.map(String).join(' ')); }

// RegExp.prototype.exec / test at one site, many times.
function px(font) {
    const m = /(\d+(?:\.\d+)?)px/.exec(font);
    return m ? Number(m[1]) : -1;
}
let sum = 0;
const fonts = ['normal 14px serif', 'bold 18.5px "x"', 'no size', '7px a'];
for (let i = 0; i < 400; i++) sum += px(fonts[i % 4]);
log('px', sum);

function quoted(s) { return /(["'])[^'"]+\1/.test(s); }
let q = 0;
for (let i = 0; i < 300; i++) q += quoted(i % 3 === 0 ? '"a"' : 'a') ? 1 : 0;
log('quoted', q);

// One site, a RegExp with an own `exec`, then without it again.
function run(re, s) { return re.exec(s); }
const plain = /b+/;
const own = /b+/;
for (let i = 0; i < 50; i++) run(plain, 'abbbc');
own.exec = function (s) { return ['own:' + s]; };
log('own', run(own, 'abbbc')[0], run(plain, 'abbbc')[0], run(own, 'x')[0]);
delete own.exec;
log('own deleted', run(own, 'abbbc')[0]);

// A global RegExp's lastIndex advances across calls at one site.
const g = /a/g;
const idx = [];
for (let i = 0; i < 5; i++) { const m = run(g, 'aXaXa'); idx.push(m ? m.index : null, g.lastIndex); }
log('global', idx.join(','));

// The same site meets a RegExp, an object with its own exec, and a subclass.
class Sub extends RegExp { exec(s) { const m = super.exec(s); return m ? ['sub:' + m[0]] : null; } }
const mixed = [/c/, { exec(s) { return ['obj:' + s]; } }, new Sub('c'), /d/y];
const seen = [];
for (let i = 0; i < 40; i++) { const m = run(mixed[i % 4], 'cd'); seen.push(m ? m[0] : 'null'); }
log('mixed', seen.slice(0, 8).join(','), seen.length);

// test() at one site over an instance with its own `test`.
const t = /z/;
function tst(re, s) { return re.test(s); }
for (let i = 0; i < 30; i++) tst(t, 'z');
t.test = () => 'mine';
log('own test', tst(t, 'z'), tst(/z/, 'z'), tst(/z/, 'y'));

// A wrong receiver for a borrowed builtin throws.
try { run({ exec: RegExp.prototype.exec }, 'a'); log('no throw'); } catch (e) { log('borrowed', e instanceof TypeError); }

// Number.prototype members at one site: every arity, short and long calls.
function hex(n) { return n.toString(16); }
function dec(n) { return n.toString(); }
function fix(n, d) { return n.toFixed(d); }
const nums = [0, 255, -1, 1.5, 16777215, 1e21, NaN, Infinity, -0, 0.1];
const hs = [], ds = [], fs = [];
for (let i = 0; i < 200; i++) {
    const n = nums[i % nums.length];
    hs.push(hex(n)); ds.push(dec(n));
    if (i < 20) fs.push(fix(n, i % 4), n.toFixed());
}
log('hex', hs.slice(0, 10).join('|'), hs.length);
log('dec', ds.slice(0, 10).join('|'));
log('fixed', fs.join('|'));
log('extra args', (255).toString(16, 'ignored'), (1.25).toFixed(1, 2, 3), (5).valueOf(1));
try { hex.call(null, { toString: 1 }); } catch (e) { log('not callable', e instanceof TypeError); }
try { (10).toString(1); } catch (e) { log('radix', e instanceof RangeError); }

// One site, receivers of every primitive kind and objects.
function str(v) { return v.toString(); }
const vals = [1, 'two', 3.5, true, [4, 5], { toString() { return 'six'; } }, 7n, Symbol('eight'), /nine/g, 10];
const strs = [];
for (let i = 0; i < 100; i++) strs.push(str(vals[i % vals.length]));
log('toString', strs.slice(0, 10).join('|'), strs.length);
function val(v) { return v.valueOf(); }
let vsum = 0;
for (let i = 0; i < 100; i++) { const v = val(i % 2 ? i : String(i)); vsum += typeof v === 'number' ? v : v.length; }
log('valueOf', vsum);

// String.prototype members with fewer arguments than parameters, and more.
function parts(s, sep, lim) { return lim === undefined ? s.split(sep) : s.split(sep, lim); }
const ps = [];
for (let i = 0; i < 60; i++) ps.push(parts('a,b,c', i % 2 ? ',' : undefined, i % 3 ? undefined : 2).length);
log('split', ps.slice(0, 6).join(','));
function tr(s) { return s.trim(); }
const ts = [];
for (let i = 0; i < 60; i++) ts.push(tr(['  a ', 'b', '\t\n c ', '', '   '][i % 5]));
log('trim', JSON.stringify(ts.slice(0, 5)));
function pad(s) { return s.padStart(); }
log('short call', pad('x'), 'abc'.concat(), 'abc'.at(), 'abc'.localeCompare(), 'abc'.repeat(), 'x'.normalize());
function at(v, i) { return v.at(i); }
const ats = [];
for (let i = 0; i < 30; i++) ats.push(at(i % 2 ? 'xyz' : [1, 2, 3], -1));
log('at', ats.slice(0, 4).join(','));
console.log(out.join('\n'));
