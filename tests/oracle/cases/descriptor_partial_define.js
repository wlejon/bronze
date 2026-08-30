// A PARTIAL property descriptor: what `Object.defineProperty` leaves alone.
//
// ECMA-262 10.1.6.3 ValidateAndApplyPropertyDescriptor is written in terms of
// the FIELDS A DESCRIPTOR HAS, not the four attributes a property carries. Step
// 5 sets "each field of Desc" and says nothing about the rest, so a descriptor
// that names only `writable` changes only writability — the value, the
// enumerability and the configurability of a live property survive it
// untouched. Only a property being CREATED gets the missing fields filled in,
// and then by step 1 through 6.2.6.6 CompletePropertyDescriptor, which fills
// them with `undefined` and false.
//
// The distinction is invisible until a descriptor is partial, which is exactly
// how a program redescribes something it did not create: `defineProperty(o, k,
// {writable: false})` is how a library seals one member of an object it was
// handed. Writing the value unconditionally turns that into a store of
// `undefined` over whatever was there — a silent data loss with no error and no
// clue, since the ATTRIBUTES all come out right.
//
// The kind is part of what survives. A descriptor with neither a data field
// (`value`, `writable`) nor an accessor field (`get`, `set`) is a GENERIC
// descriptor (6.2.6.1), and 10.1.6.3 lets one through every kind test — so
// `{enumerable: false}` on an accessor leaves an accessor, with its getter.

function show(o, k) {
    const d = Object.getOwnPropertyDescriptor(o, k);
    if (!d) return 'none';
    if ('value' in d) {
        return 'v=' + String(d.value) + ' w=' + d.writable + ' e=' + d.enumerable +
               ' c=' + d.configurable;
    }
    return 'g=' + typeof d.get + ' s=' + typeof d.set + ' e=' + d.enumerable +
           ' c=' + d.configurable;
}
function attempt(fn) {
    try {
        return fn();
    } catch (e) {
        return e instanceof TypeError ? 'TypeError' : 'other';
    }
}

// 1-4. One attribute at a time on a property an object literal created, which
// 13.2.5.5 PropertyDefinitionEvaluation makes writable, enumerable and
// configurable. Each define names one field; step 5 sets that one and the
// value stays 4.
const a = { p: 4 };
Object.defineProperty(a, 'p', { writable: false });
console.log('1', a.p, show(a, 'p'));

const b = { p: 4 };
Object.defineProperty(b, 'p', { enumerable: false });
console.log('2', b.p, show(b, 'p'), JSON.stringify(Object.keys(b)));

const c = { p: 4 };
Object.defineProperty(c, 'p', { configurable: false });
console.log('3', c.p, show(c, 'p'));

const d = { p: 4 };
Object.defineProperty(d, 'p', { writable: false, enumerable: false, configurable: false });
console.log('4', d.p, show(d, 'p'));

// 5. A descriptor with NO fields is 10.1.6.3 step 3: return true, having
// changed nothing at all.
const e = { p: 4 };
Object.defineProperty(e, 'p', {});
console.log('5', e.p, show(e, 'p'));

// 6. `value: undefined` is PRESENT, so it is applied. Presence is what step 5
// tests, never truthiness — this is the line that separates "absent" from
// "explicitly undefined" and is why the two cannot be one flag.
const f = { p: 4 };
Object.defineProperty(f, 'p', { value: undefined });
console.log('6', String(f.p), show(f, 'p'));

// 7-8. The property's KIND survives a generic descriptor (6.2.6.1): an
// accessor redescribed with `{enumerable}` or `{configurable}` alone is still
// an accessor, and both halves are still the ones that were there.
const g = {};
Object.defineProperty(g, 'p', { get() { return 'got'; }, enumerable: true, configurable: true });
Object.defineProperty(g, 'p', { enumerable: false });
console.log('7', g.p, show(g, 'p'));

const h = {};
Object.defineProperty(h, 'p', { set(v) { this.seen = v; }, enumerable: true, configurable: true });
Object.defineProperty(h, 'p', { configurable: false });
h.p = 9;
console.log('8', h.seen, show(h, 'p'));

// 9. The same, for an object a `delete` has already moved out of its shape:
// the storage differs and the algorithm may not.
const i = { p: 4, q: 5 };
delete i.q;
Object.defineProperty(i, 'p', { enumerable: false });
console.log('9', i.p, show(i, 'p'));

// 10. 20.1.2.3.1 ObjectDefineProperties step 4 is DefinePropertyOrThrow per
// key, so `defineProperties` is this algorithm N times and cannot hold a
// second opinion about a missing field.
const j = { p: 4, r: 6 };
Object.defineProperties(j, { p: { writable: false }, r: { enumerable: false } });
console.log('10', j.p, j.r, show(j, 'p'), show(j, 'r'));

// 11. And 28.1.3 Reflect.defineProperty is the same internal method, reporting
// its boolean.
const k = { p: 4 };
console.log('11', Reflect.defineProperty(k, 'p', { configurable: false }), k.p, show(k, 'p'));

// 12. A property that does NOT exist yet takes the other road: 10.1.6.3 step 1
// creates it from the descriptor completed by 6.2.6.6, so every field the
// descriptor omits is `undefined` or false. Nothing is being preserved here,
// which is the contrast the eleven lines above are about.
const l = {};
Object.defineProperty(l, 'p', { enumerable: true });
console.log('12', String(l.p), show(l, 'p'));

// 13. Partial defines compose: the value outlives both of them.
const m = { p: 'keep' };
Object.defineProperty(m, 'p', { writable: false });
Object.defineProperty(m, 'p', { enumerable: false });
console.log('13', m.p, show(m, 'p'));

// 14-16. On a NON-configurable property step 4 decides first, and it compares
// rather than refusing on presence: `{enumerable: true}` naming the value the
// property already has is allowed (4.b is an inequality), the opposite one is
// not, and neither disturbs the value.
const n = { p: 1 };
Object.defineProperty(n, 'p', { writable: false, configurable: false });
console.log('14', attempt(() => { Object.defineProperty(n, 'p', { enumerable: true }); return show(n, 'p'); }));
console.log('15', attempt(() => { Object.defineProperty(n, 'p', { enumerable: false }); return 'ok'; }));
console.log('16', n.p, show(n, 'p'));
