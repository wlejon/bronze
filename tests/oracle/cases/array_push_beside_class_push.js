// `array.push(x)` on a local Array in a program where exactly one class
// declares a `push` method (three.js's MultiDrawRenderList beside
// getTextureCacheKey): the call must reach Array.prototype.push, whatever
// the class does, and every Array rule push has must hold.
class RenderList {
    constructor() { this.items = []; this.count = 0; }
    push(x) { this.count += 1; this.items[this.items.length] = x; return -1; }
    shift() { return 'class-shift'; }
}

function cacheKey(t) {
    const array = [];
    array.push(t.wrapS);
    array.push(t.wrapT);
    array.push(t.wrapR || 0);
    array.push(t.magFilter);
    array.push(t.minFilter);
    array.push(t.anisotropy);
    array.push(t.internalFormat);
    array.push(t.format);
    array.push(t.type);
    array.push(t.generateMipmaps);
    array.push(t.premultiplyAlpha);
    array.push(t.flipY);
    array.push(t.unpackAlignment);
    array.push(t.colorSpace);
    return array.join();
}
const tex = { wrapS: 1001, wrapT: 1001, magFilter: 1006, minFilter: 1008, anisotropy: 1, internalFormat: null,
    format: 1023, type: 1009, generateMipmaps: false, premultiplyAlpha: false, flipY: true, unpackAlignment: 4,
    colorSpace: '' };
let k = '';
for (let i = 0; i < 500; i++) k = cacheKey(tex);
console.log(k);

const rl = new RenderList();
for (let i = 0; i < 5; i++) console.log(rl.push(i), rl.count);

function lengths() {
    const a = [];
    const out = [];
    for (let i = 0; i < 20; i++) out.push(a.push(i * 2));
    return out.join(' ') + ' | ' + a.join(',');
}
console.log(lengths());

function objects() {
    const a = [];
    for (let i = 0; i < 6; i++) a.push({ i });
    return a.map((o) => o.i).join('');
}
console.log(objects());

function shifts() {
    const a = [1, 2, 3];
    a.push(4);
    return [a.shift(), a.shift(), a.length].join(',');
}
console.log(shifts());

// a frozen array: push throws a TypeError
function frozen() {
    const a = [1];
    Object.freeze(a);
    try { a.push(2); return 'no throw'; } catch (e) { return e instanceof TypeError; }
}
console.log(frozen());

// a setter on the array's own prototype at the next index takes the store
function withSetter() {
    const proto = Object.create(Array.prototype);
    Object.defineProperty(proto, '3', { set(v) { console.log('setter', v); }, get() { return 'proto'; } });
    const a = [0, 1, 2];
    Object.setPrototypeOf(a, proto);
    a.push(9);
    return a.length + ' ' + a[3];
}
console.log(withSetter());

// a prototype between the array and Array.prototype with a push of its own
function shadowed(withOwn) {
    const a = [];
    if (withOwn) {
        const proto = Object.create(Array.prototype);
        proto.push = function (x) { return 'shadowed ' + x; };
        Object.setPrototypeOf(a, proto);
    }
    return a.push(5) + ' ' + a.length;
}
console.log(shadowed(true));
console.log(shadowed(false));

// the class's push copied onto an array instance
function ownPush() {
    const a = [];
    a.push = RenderList.prototype.push;
    a.count = 0;
    a.items = [];
    return a.push(7) + ' ' + a.count + ' ' + a.length;
}
console.log(ownPush());
