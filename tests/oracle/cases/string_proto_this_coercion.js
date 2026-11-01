// Every String.prototype member except toString/valueOf takes its receiver
// through RequireObjectCoercible + ToString (ECMA-262 22.1.3): a number, a
// boolean, an array, a plain object with toString, a String wrapper (through
// its own toString when it has one) are converted; null and undefined throw a
// TypeError; a Symbol throws from ToString. toString/valueOf stay
// thisStringValue and refuse anything that is not a string or a wrapper.

function show(v) {
  if (typeof v === 'string') return JSON.stringify(v);
  if (Array.isArray(v)) return '[' + v.map(show).join(',') + ']';
  if (v !== null && typeof v === 'object') {
    if (typeof v.next === 'function') {
      const parts = [];
      for (let r = v.next(); !r.done; r = v.next()) parts.push(show(r.value));
      return 'iter<' + parts.join(',') + '>';
    }
    return 'obj:' + JSON.stringify(v);
  }
  return String(v);
}

function run(fn) {
  try {
    return show(fn());
  } catch (e) {
    return e instanceof TypeError ? 'TypeError' : 'threw ' + e;
  }
}

const log = [];
const counted = {
  toString() {
    log.push('toString');
    return 'Abc Def';
  },
};
const wrapperOwn = new String('wrapped');
wrapperOwn.toString = function () {
  return 'OWN';
};

const receivers = [
  ['number', 12345678e3],
  ['negzero', -0],
  ['nan', NaN],
  ['bool', true],
  ['array', ['ab', 'cd']],
  ['object', counted],
  ['wrapper', new String('Wrap')],
  ['wrapperOwn', wrapperOwn],
  ['prim', { [Symbol.toPrimitive](hint) { return 'Prim:' + hint; } }],
  ['null', null],
  ['undefined', undefined],
  ['symbol', Symbol('s')],
  ['string', ' sTr ing '],
];

const P = String.prototype;
const calls = [
  ['at', (r) => P.at.call(r, 1)],
  ['charAt', (r) => P.charAt.call(r, 2)],
  ['charCodeAt', (r) => P.charCodeAt.call(r, 0)],
  ['codePointAt', (r) => P.codePointAt.call(r, 1)],
  ['concat', (r) => P.concat.call(r, '|', 1)],
  ['endsWith', (r) => P.endsWith.call(r, '0')],
  ['includes', (r) => P.includes.call(r, 'b')],
  ['indexOf', (r) => P.indexOf.call(r, '4')],
  ['isWellFormed', (r) => P.isWellFormed.call(r)],
  ['lastIndexOf', (r) => P.lastIndexOf.call(r, 'e')],
  ['localeCompare', (r) => P.localeCompare.call(r, typeof r === 'symbol' ? '' : String(r))],
  ['match', (r) => P.match.call(r, /[a-z]+/i)],
  ['matchAll', (r) => [...P.matchAll.call(r, /\d/g)].map((m) => m[0])],
  ['normalize', (r) => P.normalize.call(r)],
  ['padEnd', (r) => P.padEnd.call(r, 14, '*')],
  ['padStart', (r) => P.padStart.call(r, 14, '-')],
  ['repeat', (r) => P.repeat.call(r, 2)],
  ['replace', (r) => P.replace.call(r, 'e', 'E')],
  ['replaceAll', (r) => P.replaceAll.call(r, 'a', '_')],
  ['search', (r) => P.search.call(r, /[0-9]/)],
  ['slice', (r) => P.slice.call(r, 1, -1)],
  ['split', (r) => P.split.call(r, 'e')],
  ['splitRe', (r) => P.split.call(r, /[,e]/)],
  ['startsWith', (r) => P.startsWith.call(r, '1')],
  ['substr', (r) => P.substr.call(r, 1, 3)],
  ['substring', (r) => P.substring.call(r, 3, 1)],
  ['toLowerCase', (r) => P.toLowerCase.call(r)],
  ['toUpperCase', (r) => P.toUpperCase.call(r)],
  ['toLocaleLowerCase', (r) => P.toLocaleLowerCase.call(r)],
  ['toLocaleUpperCase', (r) => P.toLocaleUpperCase.call(r)],
  ['toWellFormed', (r) => P.toWellFormed.call(r)],
  ['trim', (r) => P.trim.call(r)],
  ['trimEnd', (r) => P.trimEnd.call(r)],
  ['trimStart', (r) => P.trimStart.call(r)],
  ['iterator', (r) => P[Symbol.iterator].call(r)],
  ['toString', (r) => P.toString.call(r)],
  ['valueOf', (r) => P.valueOf.call(r)],
];

for (const [cname, call] of calls) {
  const row = [];
  for (const [rname, r] of receivers) row.push(rname + '=' + run(() => call(r)));
  console.log(cname + ': ' + row.join(' '));
}
console.log('object toString calls: ' + log.length);

// The receiver is converted once per call, before the arguments.
const order = [];
const recv = {
  toString() {
    order.push('this');
    return 'xyz';
  },
};
const arg = {
  toString() {
    order.push('arg');
    return 'y';
  },
};
console.log(P.indexOf.call(recv, arg), P.includes.call(recv, arg), P.padStart.call(recv, 5, arg));
console.log(order.join(','));

// A pattern argument's [Symbol.*] method gets the receiver UNCONVERTED, and
// the receiver's toString runs only if the member's own algorithm does.
order.length = 0;
const pattern = {
  [Symbol.replace](s, r) {
    return 'replace:' + typeof s + ':' + r;
  },
  [Symbol.split](s, lim) {
    return 'split:' + typeof s + ':' + lim;
  },
  [Symbol.search](s) {
    return 'search:' + typeof s;
  },
  [Symbol.match](s) {
    return 'match:' + typeof s;
  },
};
console.log(P.replace.call(recv, pattern, 'R'));
console.log(P.split.call(recv, pattern, 3));
console.log(P.search.call(recv, pattern));
console.log(P.match.call(recv, pattern));
console.log('receiver conversions: ' + order.length);

// replace: the search value is converted before the replacement.
order.length = 0;
const sv = {
  toString() {
    order.push('search');
    return 'b';
  },
};
const rv = {
  toString() {
    order.push('replacement');
    return 'B';
  },
};
console.log(P.replace.call(recv, sv, rv), P.replaceAll.call(12121, 1, 'one'));
console.log(order.join(','));

// A string receiver keeps every fast path, and the answer of a member that
// returns its receiver unchanged is that string.
const s = 'already lower';
console.log(s.toLowerCase() === s, s.normalize() === s, s.toWellFormed() === s);
let acc = 0;
for (let i = 0; i < 2000; i++) {
  const t = String(i);
  acc += t.charCodeAt(0) + t.indexOf('1') + t.toUpperCase().length + P.slice.call(i, 1).length;
}
console.log(acc);
