// `console.warn` and `console.error`. three.js calls them 91 and 62 times
// against 20 calls to `console.log`, from ordinary code paths — `Material` and
// `Object3D` both do — so a build of anything three.js-shaped produces library
// chatter, and where that chatter goes decides whether the oracle can keep
// pinning stdout byte-for-byte at all.
//
// So the load-bearing assertion of this case is a NEGATIVE one, and it is the
// pinned file itself: every `console.warn` and `console.error` below writes
// something distinctive, and NONE of it appears in console_streams.expected.
// If a later refactor merges the two streams, this case fails — which is the
// only way that mistake gets caught, because a merged stream is otherwise
// indistinguishable from working code.
//
// From the WHATWG Console Standard §2.1 and §2.2 (ECMA-262 does not define
// `console`), and the inspect format for the formatting:
//
// 1. `log`, `info` and `debug` are one operation on stdout: §2.1 defines all
//    three as Logger with a different severity hint, and a severity hint is
//    something a terminal filter reads, not something a compiler emits.
// 2. `warn` and `error` are the same operation on stderr.
// 3. The FORMATTING is identical on both streams: the same inspect format,
//    the same single-space join between arguments, the same one newline.
//    They share one formatter in the runtime precisely so the two can never
//    drift.
// 4. Every one of them evaluates to `undefined`, like any call that returns
//    nothing.
//
// DELIBERATE DIVERGENCE: `console.trace` and every other member is a compile
// error naming itself rather than `undefined`, because a stack trace is not
// something bronze can build. That refusal is pinned in
// tests/parse/parser_expr_test.cpp, where a compile error can be observed.

// 1 — the stdout three.
console.log('log reaches stdout');
console.info('info reaches stdout');
console.debug('debug reaches stdout');

// 2 — the stderr two. Neither line may appear in the pinned bytes.
console.warn('WARN-MUST-NOT-REACH-STDOUT');
console.error('ERROR-MUST-NOT-REACH-STDOUT');

// 3 — the same containers through both streams. Only the stdout one is
// pinned; the stderr one is here so that a merged stream shows up as two
// copies of the line rather than as nothing.
const shape = { name: 'material', side: 2, layers: [1, 2] };
console.warn('WARN-CONTAINER', shape, [1, 'two', undefined]);
console.log('LOG-CONTAINER', shape, [1, 'two', undefined]);

// 4 — each returns undefined, and the argument is evaluated before the call
// that consumes it, so the stderr write happens first and only `undefined`
// lands on stdout.
console.log(console.warn('WARN-INSIDE-A-LOG'));
console.log(console.log('nested log argument'));

// A method that warns is an ordinary method: nothing about the receiver or
// the call changes, which is the shape three.js's `Material.setValues` has.
class Material {
    constructor(name) {
        this.name = name;
    }

    setValue(key, value) {
        if (value === undefined) {
            console.warn('WARN-FROM-A-METHOD', this.name, key);
            return this;
        }
        this[key] = value;
        return this;
    }
}

const m = new Material('basic');
m.setValue('opacity', 0.5).setValue('missing', undefined);
console.log(m.name, m.opacity);
console.log(Object.keys(m));
console.log('the last line on stdout');
