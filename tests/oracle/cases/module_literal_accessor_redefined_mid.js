// The same module-scope object literal, redefined BETWEEN two reads.
//
// The companion case redefines before anything reads, so nothing can have been
// decided yet. Here a thousand reads have already happened and been answered
// one way when `Object.defineProperty` replaces the accessor, and the reads
// after it — at the same sites, in the same loops — have to answer the other
// way. A conclusion that outlives the property it was drawn from shows up here
// and nowhere else.

const Space = {
  _working: 'srgb-linear',
  get working() { return this._working; },
  set working(v) { this._working = v; }
};

function readOnce() { return Space.working; }

let before = 0;
for (let i = 0; i < 1000; i++) {
  if (readOnce() === 'srgb-linear') before++;
}
console.log('before        ' + Space.working + ' ' + before);

// 10.1.6.3 step 4 copies only the fields present, so `set` survives again; what
// changes is the getter, and it stops reading `_working` altogether.
let redefCalls = 0;
Object.defineProperty(Space, 'working', {
  get() { redefCalls++; return 'REDEF'; },
  configurable: true,
  enumerable: true
});

let after = 0;
for (let i = 0; i < 1000; i++) {
  if (readOnce() === 'REDEF') after++;
}
console.log('after         ' + Space.working + ' ' + after);
console.log('getter calls  ' + redefCalls);
console.log('backing       ' + Space._working);

// The surviving setter still writes the backing property, which the redefined
// getter no longer reads — so the two answers part company, and that is what
// says the setter really ran.
Space.working = 'written';
console.log('after set     ' + Space.working + ' ' + Space._working);

// Put the original getter back, at the same site, and the site answers the
// original way again.
Object.defineProperty(Space, 'working', {
  get() { return this._working; },
  configurable: true,
  enumerable: true
});

let restored = 0;
for (let i = 0; i < 1000; i++) {
  if (readOnce() === 'written') restored++;
}
console.log('restored      ' + Space.working + ' ' + restored);
