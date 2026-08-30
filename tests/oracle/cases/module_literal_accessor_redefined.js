// A module-scope object literal whose accessor is redefined BEFORE anything
// reads it.
//
// The literal is the shape a compiler most wants to reason about — a `const` at
// module scope, an accessor whose body is one read of a sibling data property —
// and `Object.defineProperty` takes it away before the first read happens.
// Every read here must be the accessor that is installed NOW, so the answer
// names the redefinition and never the literal's own getter.
//
// The companion case `module_literal_accessor_redefined_mid` puts the same
// redefinition BETWEEN two reads instead, which is the harder half: a read has
// already happened by then.

const Space = {
  _working: 'srgb-linear',
  get working() { return this._working; },
  set working(v) { this._working = v; }
};

// 10.1.6.3 ValidateAndApplyProperty step 4 copies only the fields the
// descriptor HAS. This one has no `set`, so the literal's own setter survives
// the redefinition and only the getter is replaced — which the writes below
// pin, because a lost setter would make them stores instead.
Object.defineProperty(Space, 'working', {
  get() { return 'redefined-' + this._working; },
  configurable: true,
  enumerable: true
});

console.log('first read    ' + Space.working);
console.log('bracket       ' + Space['working']);
console.log('backing       ' + Space._working);

let hot = 0;
for (let i = 0; i < 1000; i++) {
  if (Space.working === 'redefined-srgb-linear') hot++;
}
console.log('hot           ' + hot);

// The backing property is still an ordinary data property and the replacement
// getter reads it, so writing it moves what the accessor answers.
Space._working = 'display-p3';
console.log('after write   ' + Space.working + ' ' + Space._working);

// The surviving setter, reached through the redefined property.
Space.working = 'through-setter';
console.log('after set     ' + Space.working + ' ' + Space._working);

const d = Object.getOwnPropertyDescriptor(Space, 'working');
console.log('descriptor    ' + (typeof d.get) + ' ' + (typeof d.set) + ' ' +
            d.enumerable + ' ' + d.configurable);
