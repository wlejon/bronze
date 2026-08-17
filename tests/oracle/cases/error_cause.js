// The `cause` option of the error constructors (ECMA-262 20.5.1.1 step 4 and
// 20.5.8.1 InstallErrorCause).
//
// Derived from ECMA-262:
//
// 1. 20.5.8.1 step 1 is `HasProperty(options, "cause")`, so the property is
//    installed only when the options object HAS a `cause` — and installed even
//    when its value is `undefined`. `'cause' in new Error("m", {})` is
//    therefore false while `'cause' in new Error("m", {cause: undefined})` is
//    true, and that distinction is the whole reason the step is HasProperty and
//    not "is the read undefined".
// 2. Step 1.b is CreateNonEnumerableDataPropertyOrThrow, so `cause` is an own,
//    non-enumerable property: `Object.keys(err)` stays empty and a for-in over
//    an error visits nothing, while `Object.getOwnPropertyNames` reports it.
// 3. A non-object second argument is ignored entirely (step 1's condition), so
//    `new Error("m", "cause")` has no `cause` at all.
// 4. Every native error class takes the same options argument in the same
//    position — 20.5.6.1.1 gives the NativeError classes the identical step —
//    except `AggregateError` (20.5.7.1.1), whose options are the THIRD argument
//    because `errors` comes first.
// 5. The message and the cause are independent: `new Error(undefined, {cause})`
//    has a cause and no own `message`, because step 3 leaves `message` off for
//    an undefined first argument while step 4 still runs.
// 6. HasProperty walks the prototype chain, so an options object that INHERITS
//    `cause` installs it.
// 7. `cause` is an ordinary writable property once installed, and it is not
//    inherited by anything: an error built without the option has no `cause`
//    anywhere on its chain.
const withCause = new Error('m', { cause: 42 });
console.log('cause' in withCause, withCause.cause);
console.log(Object.keys(withCause).length);
console.log(Object.getOwnPropertyNames(withCause).sort().join(','));
console.log(withCause.message, withCause.name);

// 1: presence, not truthiness.
console.log('cause' in new Error('m'));
console.log('cause' in new Error('m', {}));
console.log('cause' in new Error('m', { cause: undefined }));
console.log(new Error('m', { cause: undefined }).cause);
console.log('cause' in new Error('m', { cause: null }), new Error('m', { cause: null }).cause);
console.log(new Error('m', { cause: 0 }).cause, new Error('m', { cause: false }).cause);

// 2: for-in sees nothing.
const forIn = [];
for (const k in withCause) forIn.push(k);
console.log(forIn.length);

// 3: a non-object options argument.
console.log('cause' in new Error('m', 'cause'));
console.log('cause' in new Error('m', 7));
console.log('cause' in new Error('m', null));

// 4: every class.
console.log(new TypeError('t', { cause: 'T' }).cause);
console.log(new RangeError('r', { cause: 'R' }).cause);
console.log(new SyntaxError('s', { cause: 'S' }).cause);
console.log(new ReferenceError('f', { cause: 'F' }).cause);
console.log(new URIError('u', { cause: 'U' }).cause);
const agg = new AggregateError([1, 2], 'a', { cause: 'A' });
console.log(agg.cause, agg.message, agg.errors.join(','));
console.log('cause' in new AggregateError([], 'a'));
console.log('cause' in new AggregateError([], 'a', {}));

// The constructor called WITHOUT `new` behaves the same (20.5.1.1 step 1).
console.log(Error('m', { cause: 'plain' }).cause);
console.log(TypeError('t', { cause: 'plainT' }).cause);

// 5: message and cause are independent.
const noMessage = new Error(undefined, { cause: 'only' });
console.log(noMessage.cause, noMessage.message === '', 'message' in noMessage);
console.log(Object.getOwnPropertyNames(noMessage).join(','));

// 6: an inherited `cause`.
const base = { cause: 'inherited' };
console.log(new Error('m', Object.create(base)).cause);

// A getter runs, once, and its value is what lands.
let reads = 0;
const err = new Error('m', { get cause() { reads += 1; return `read${reads}`; } });
console.log(err.cause, reads);

// 7: a plain error, and writability.
const plain = new Error('m');
console.log(plain.cause, 'cause' in plain);
withCause.cause = 'replaced';
console.log(withCause.cause);

// A cause that is an error is the whole point of the option.
const inner = new RangeError('inner');
const outer = new Error('outer', { cause: inner });
console.log(outer.cause === inner, outer.cause.name, outer.cause.message);
console.log(String(outer));
