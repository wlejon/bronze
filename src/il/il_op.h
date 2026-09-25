#pragma once

#include <cstdint>

namespace bronze::il {

enum class Op : uint8_t {
    ConstF64,   // a = const.f64 <imm>
    ConstI32,   // a = const.i32 <imm>
    ConstBool,  // a = const.bool <imm>
    ConstUndefined, // a: dynamic = const.undefined
    ConstNull,      // a: dynamic = const.null
    // A BigInt literal. `keyIndex` names its SOURCE TEXT in the key pool, not a
    // payload: a BigInt has no width, so there is no immediate field the value
    // would fit in, and the text is what the compiler already knows how to hand
    // the runtime. The result is a boxed heap value like every other BigInt —
    // there is no unboxed BigInt anywhere in the IL, which is exactly what
    // keeps a typed f64 path from ever meeting one.
    ConstBigInt,    // a: dynamic = const.bigint <key_const_index>
    Add,        // a = add b, c        (numeric, operands same type)
    // A `+` spine of three or more operands as an ACCUMULATOR: each IS the
    // `add` it replaces, at the same point, so 13.15.3's order is untouched, and
    // a String result is carried between them as ONE growable allocation sized
    // by `immI32` on begin. See lower_concat_chain.cpp, bronze_abi.h, verifier.
    ConcatBegin,   // a: dynamic = concat.begin b, c, <remaining>
    ConcatAppend,  // a: dynamic = concat.append b, c
    ConcatEnd,     // a: dynamic = concat.end b
    Sub,
    // Unary minus, which is NOT `0 - x`: IEEE-754 says 0 - 0 is +0,
    // while negation flips the sign bit, so `-0` was printing as 0.
    Neg,        // a: f64 = neg b
    Mul,
    Div,
    Mod,
    // JS `**`, which is NOT C's pow: ECMA-262 Number::exponentiate makes an
    // exponent of NaN yield NaN even for base 1, and a base of magnitude 1
    // with an infinite exponent yield NaN. Its own op so the one algorithm
    // serves `**` and `Math.pow` alike.
    Pow,        // a: f64 = pow b, c
    // The bitwise family. Operands are i32 — lowering puts a `to.int32` in
    // front of every one of them — and the RESULT is the JS number that int32
    // denotes, so these read `%n: f64 = and %i32, %i32`. Keeping the result an
    // i32 would leak a type inference has no element for into block joins and
    // calling conventions; the int32 is an intermediate of the operator and
    // never escapes it.
    // 7.1.3 ToNumeric over a boxed value: a Number or a BigInt out, still
    // boxed. Not `unbox.f64`, which is ToNumBER and refuses a BigInt — and the
    // difference is observable, because a POSTFIX update yields this value.
    ToNumeric,  // a: dynamic = to.numeric b
    // `++` and `--`, whose delta has the operand's own type: 1 for a Number,
    // 1n for a BigInt. `immI32` is 1 for an increment. It is an instruction
    // rather than an `add` against a constant because there is no constant
    // that would be right for both types — a Number 1 against a BigInt is the
    // mixing TypeError.
    NumericStep,// a: dynamic = numeric.step b, <+1|-1>
    ToInt32,    // a: i32 = to.int32 b        (b: f64, bool or dynamic)
    BitAnd,     // a: f64 = and b, c
    BitOr,
    BitXor,
    MathImul,   // a: f64 = imul b, c         (operands i32, 32-bit signed multiply denotated as f64)
    Shl,        // a: f64 = shl b, c          (count masked to 5 bits)
    Shr,        // arithmetic: the sign bit is replicated
    UShr,       // logical, and the ONE bitwise op whose result is ToUint32
    // `~x` over a BOXED operand, and only over one. On numbers `~x` is `x ^ -1`
    // and lowering still spells it that way; on a BigInt that spelling is a
    // MIXING TypeError, because -1 is a Number. So the op exists for the case
    // where the operand's type is not known — never for a proven-numeric one.
    BitNot,     // a: dynamic = bitnot b      (b: dynamic)
    CmpLt,      // a: bool = cmp.lt b, c
    CmpGt,
    // The ORDERED `<=` and `>=` on numbers, which are not `!(a > b)` and
    // `!(a < b)`: that identity needs a total order and NaN does not give one.
    // ECMA-262 13.10 answers false when IsLessThan produces *undefined* —
    // 13.10.1 step 4.c, either operand NaN — and a negation maps that same
    // undefined to true, so `NaN <= 1` came out yes. These two answer false for
    // NaN the way cmp.lt and cmp.gt already did.
    CmpLe,      // a: bool = cmp.le b, c
    CmpGe,
    CmpEq,
    // The exact negation of CmpEq, NaN included: `NaN != NaN` is true, so on
    // doubles this is the UNORDERED compare. Numeric truthiness is a
    // different question with the same shape and has its own op below —
    // conflating them printed `NaN !== NaN` as false.
    CmpNe,
    // ToBoolean of a number: neither zero nor NaN, which is the ORDERED
    // "not equal to 0". Named rather than spelled `cmp.ne x, 0` because the
    // two differ exactly at NaN and every use of one is a wrong answer for
    // the other.
    NumTruthy,  // a: bool = num.truthy b     (b: f64)
    StrictEq,   // a: bool = strict.eq b, c   (JS ===, both operands dynamic)
    LooseEq,    // a: bool = loose.eq b, c    (JS ==, both operands dynamic)
    // The four relational operators over BOXED operands: ECMA-262 13.10 and
    // 13.10.1 IsLessThan entire, which no compare instruction implements. Step
    // 3 asks whether both operands are Strings after ToPrimitive and, if they
    // are, compares them by CODE UNIT and converts nothing — so `"2" < "10"` is
    // true where `2 < 10` is false. ToNumeric is step 4, the else-branch, and
    // reaching for it first is how two strings came to be compared as NaNs.
    //
    // Four named ops rather than one carrying an operator field, for the reason
    // `print.err` is not a flag on `print`: the canonical dump is what a reader
    // bisects with, and which comparison this is, is exactly the kind of fact a
    // field can silently omit.
    RelLt,      // a: bool = rel.lt b, c      (both operands dynamic)
    RelGt,
    RelLe,
    RelGe,
    TypeOf,     // a: dynamic = typeof b      (one of eight strings)
    // ECMA-262 7.1.17 ToString, step 1 included: an OBJECT operand is
    // ToPrimitive'd with hint STRING first, so `toString` is tried before
    // `valueOf`. It is deliberately not spelled `"" + b`: 13.15.3 asks
    // ToPrimitive for NO hint, which reverses that pair — so `+` and this
    // give different answers for an object defining both, and a template
    // substitution is ToString (13.2.8.6) rather than a concatenation.
    ToStr,      // a: dynamic = to.string b
    InstanceOf, // a: bool = instanceof b, c
    In,         // a: bool = in b, c          (b: key, c: object)
    IsNullish,  // a: bool = is.nullish b
    // The NUMBER TEST, as a value rather than as a throw. `bits <= NUMBER_MAX`
    // — a Number's bits are its double's bits and every other tag sits above
    // the number range (`bronze_abi.h`) — so this is one unsigned compare that
    // reads no memory, calls nothing and cannot raise.
    //
    // It is the same compare `pin.guard` makes, and the difference is the whole
    // point of it. `pin.guard` holds a program to a promise and THROWS when the
    // promise is broken; this one is a BRANCH, and what its false edge leads to
    // is ordinary IL that would have existed anyway. The guarded-region pass
    // (src/lower/guard_region.h) emits it as the condition of a block's
    // terminator, and the block on the true edge is where the `unbox.f64 raw`
    // it licenses lives.
    IsNumber,   // a: bool = is.number b     (b: dynamic)
    // The DENSE-ARRAY TEST: does this receiver answer every constant-index read
    // up to `immI32` out of its own element block, with no user code and no
    // allocation? It is to a constant-index read what `is.number` is to a
    // coercion, so one test covers a run of reads. It compiles to the backend's
    // own receiver proof (llvm_recv_proof.h), so it claims nothing the reads it
    // stands in front of were not already going to assume; the pass in
    // `src/lower/guard_region.h` is its only producer and says why.
    IsDenseArray,  // a: bool = is.dense_array b, <immI32: max index>
    Ret,        // ret [a]
    // `throw v`: goes to this block's handler with v, or raises v out of the
    // function when the block has none. A terminator, because it is a way OUT
    // of the block like a jump — the edge it takes is just written on the
    // block rather than on the instruction.
    Throw,      // throw a
    // The thrown value a handler block was entered with. The first
    // instruction of every handler block and the only way to read it; a
    // `finally` holds it while its body runs and then decides whether to
    // raise it again.
    ExcTake,    // a: dynamic = exc.take
    Jump,       // jump bN(args...)
    Branch,     // br %cond, bThen(args...), bElse(args...)
    Call,       // a = call <funcRef>(args...)
    Box,        // a = box.<type> b
    Unbox,      // a = unbox.<type> b
    PropGet,    // a = prop.get b, <key_const_index>, <ic_site_index>
    // `super.k`: a read of the PARENT prototype's property with `this` as the
    // receiver. Identical to prop.get for a method — the value is the same
    // function either way — and not identical at all for an accessor, whose
    // getter would otherwise run with the prototype as its receiver. No inline
    // cache: the receiver and the holder are different objects, and an entry
    // describes one shape.
    SuperGet,   // a = super.get proto, <key_const_index>, thisArg
    // `immI32` is the strict flag, on the same rule prop.set below carries one:
    // `super.k = v` is an ordinary Reference, so a refused Set raises out of it
    // in strict code and is discarded in sloppy code.
    SuperSet,   // super.set proto, <key_const_index>, thisArg, val, <strict>
    // `immI32` is 1 when the reference this write goes through is STRICT
    // (ECMA-262 11.2.2 decides which code is; 13.15.2 PutValue step 6.d is what
    // reads it). It is the whole difference between a refused Set — a
    // getter-only property, a non-writable one, a non-extensible receiver —
    // raising a TypeError and being discarded, so it is carried per
    // INSTRUCTION: a module holds strict and sloppy bodies at once, and both
    // reach the same helper.
    PropSet,    // prop.set b, <key_const_index>, c, <ic_site_index>, <strict>
    ElemGet,    // a = elem.get obj, idx        (both dynamic; computed index)
    ElemSet,    // elem.set obj, idx, val, <strict>   (all dynamic)
    // Element access on a receiver INFERENCE PROVED is a Float64Array or
    // Float32Array view (`immI32` is the types::TypedArrayElem number). The
    // proof is what licenses generated code to skip the tag/kind guard ladder
    // and touch the bytes directly; the index and bounds checks stay, because
    // they are the language's own rule, not a guess about the receiver.
    //
    // The get computes ToNumber of the language's read: a valid in-bounds
    // index yields the element, and every other number index — negative,
    // fractional, NaN, past the view — yields NaN, which IS
    // ToNumber(undefined). An f64 cannot carry `undefined` itself, so
    // lowering only places this op where the consumer coerces (arithmetic, a
    // typed store's value, an update); anywhere the raw value could be
    // observed keeps `elem.get`. The bound is the view's length, the same
    // bound bronze_elem_get uses, so the two modes agree byte for byte —
    // detach included: the runtime zeroes a stranded view's length at the
    // `transfer`/`resize` that strands it (closeOrReopenViews), so the one
    // bounds compare is also 10.4.5.9's out-of-bounds check.
    //
    // The set is the full 23.2.5 store: ToNumber already done (the value
    // operand is f64), a valid index stores with the element kind's
    // narrowing, an invalid one is a silent no-op. Neither op can throw and
    // neither can allocate, which is what keeps a loop of them free of
    // safepoints.
    // <immI32> is a types::TypedArrayElem number, or the PROBE kind below.
    ElemGetTyped, // a: f64 = elem.get.typed obj, idx(f64), <immI32: elem kind>
    ElemSetTyped, // elem.set.typed obj, idx(f64), val(f64), <immI32: elem kind>
    // A call INFERENCE PROVED reaches a pristine builtin `Math` method
    // (`immI32` is the MathUnaryFn number), with its one argument already a
    // machine number. Only the BIT-EXACT functions are admitted — the ones
    // IEEE 754 pins to a single result, so the intrinsic the backend emits
    // and the libm call the runtime helper makes cannot disagree in any bit,
    // which is what keeps the two inference modes byte-identical. `sin`,
    // `pow` and friends stay dynamic calls: their results are
    // implementation-defined and MUST keep coming from the one runtime
    // kernel. Cannot throw, cannot allocate, runs no user code.
    MathUnary,  // a: f64 = math.unary x(f64), <immI32: MathUnaryFn>
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
    Construct,  // a = new callee, args...
    CreateObject, // a = create.object
    // A GENERATOR OBJECT (ECMA-262 27.5.1): %GeneratorPrototype% for a
    // prototype, and the RESUME FUNCTION its operand names for a body. Its own
    // op and not `create.object` plus two writes, because neither of the two
    // things that make it one is a property: the prototype lives on the shape,
    // and the resume closure is an internal slot.
    CreateGeneratorObject, // a = create.generator_object b
    CreateAsyncGeneratorObject, // a = create.async_generator_object b
    // The three edges of an ASYNC function (ECMA-262 27.7): make the machine
    // the runtime driver holds (operand = the resume closure), start it (run
    // the body synchronously to the first await, 27.7.5.1, and answer the
    // promise), and subscribe one await (machine, awaited value). Their own
    // ops rather than dynamic calls to named globals because — like
    // `create.generator_object` — what they touch are internal slots the
    // program can neither read nor forge.
    CreateAsyncMachine, // a = create.async_machine b     (b = resume closure)
    AsyncStart,         // a = async.start b              (b = machine; a = promise)
    AsyncAwait,         // async.await machine, value     (no result; subscribes)
    DynamicImport,      // a = dynamic_import specifier, <url_const_index of the importer>
    // A MODULE NAMESPACE EXOTIC OBJECT (ECMA-262 10.4.6), built from the object
    // of getters the operand holds. Its own op for the reason
    // `create.generator_object` is one: what it produces is not an object
    // literal with a different attribute set, it is a receiver KIND whose
    // own-key order, [[Set]] and [[GetOwnProperty]] are each its own.
    ModuleNamespace, // a = module.namespace b
    ObjectKeys, // a = object.keys b
    // The keys a `for-in` will visit, as one array built before the first
    // iteration: own AND inherited enumerable string keys, each once.
    // Snapshotting is what lets the loop itself be for-of's index walk over the
    // result, and it is a legal answer to the spec's open question about
    // mutation during enumeration.
    ForInKeys,  // a = forin.keys b
    // A class method: a property write with `enumerable: false`, which an
    // ordinary `prop.set` cannot express and which is what keeps a method out
    // of `Object.keys` and `for-in`. No IC index — a class body runs once.
    MethodDef,  // method.def obj, <key_const_index>, v
    // The same definition with a key that is a VALUE rather than a compile-time
    // constant — `class C { [Symbol.iterator]() {} }`. Its own op and not
    // `elem.set`, because a method is `enumerable: false` (15.7.14) and an
    // assignment cannot say that; and not `method.def`, because there is no
    // key constant to name. The key is whatever the expression evaluated to,
    // which for the one spelling bronze admits is the well-known symbol.
    MethodDefComputed,  // method.def.computed obj, key, v
    AccessorDef,  // accessor.def obj, <key_const_index>, getter, setter, <enumerable>
    AccessorDefComputed, // accessor.def.computed obj, key, getter, setter, <enumerable>
    // One key of an `Object.defineProperties(o, {...})` whose descriptors were
    // all object LITERALS, so what each of them says is a compile-time fact.
    // `immI32` is that fact — the BRONZE_ABI_DESC_* mask of which of `value`,
    // `writable`, `enumerable` and `configurable` the literal wrote and what
    // the three booleans were — and the operand is the `value` expression,
    // already evaluated. The descriptor object is never built and 6.2.6.5 never
    // runs; the helper hands the same decoded fields to the same 10.1.6.3 the
    // generic member reaches, so a refusal is the same TypeError in the same
    // place (src/lower/lower_define_props.cpp).
    DefineOwnAttr,  // define.own.attr obj, <key_const_index>, v, <attr_mask>
    GetNewTarget, // a = get.new_target
    // `import.meta` (13.3.12). `keyIndex` names the module's URL, which
    // lowering resolved from the file id the linker stamped on the expression
    // — so the URL is a compile-time constant and the op carries no operand.
    // The runtime answers the SAME object every time for one index, which is
    // what makes `import.meta === import.meta` hold within a module.
    ImportMeta,  // a = import.meta <url_const_index>
    // `delete o.k` and `delete o[i]`. A reference operation, not a read:
    // the operand's property is never loaded, and the result is the boolean
    // ECMA-262 13.5.1 defines rather than the property's value.
    // `immI32` is the strict flag, on the same rule prop.set carries one:
    // 13.5.1.2 step 5.b turns a delete that answers false into a TypeError,
    // but only for strict code.
    PropDelete,  // a: bool = prop.delete obj, <key_const_index>, <strict>
    ElemDelete,  // a: bool = elem.delete obj, idx, <strict>
    GlobalGet,  // a = global.get <key_const_index>
    // A name lowering could not resolve to anything, EVALUATED. Raises
    // ReferenceError at run time rather than refusing the program at compile
    // time, because what a free name denotes is a fact only the running
    // environment holds. Not a terminator: it is a helper call like any other,
    // and the block's handler is what the backend's exception test after it
    // branches to.
    // a: dynamic = name.resolve <key_const_index>. The name lowering's
    // closed ladder could not resolve: a PROPERTY OF THE GLOBAL OBJECT if the
    // program made one, and a ReferenceError otherwise (6.2.5.5 GetValue step
    // 2). It cannot be settled at compile time, because `globalThis.x = 1`
    // creates the binding a later free `x` reads.
    ResolveName,
    // An assignment to an IMMUTABLE binding, in STRICT code. 9.1.1.1.5
    // SetMutableBinding step 4 throws a TypeError there and returns quietly
    // otherwise, so sloppy code emits nothing at all and this instruction is
    // the strict half alone. The one immutable binding bronze creates is a
    // named function expression's own name (15.2.5). Not a terminator, for
    // the reason name.resolve is not.
    ImmutableAssign,  // a: dynamic = immutable.assign <key_const_index>
    // The WRITE BARRIER for a `--pins` claim (src/types/pins.h, stage B1).
    //
    // `pin.guard v, <key_const_index>, <immI32: PinBarrierKind>` tests one
    // boxed value against the shape a pin PROMISED and raises a TypeError
    // naming the manifest line when it does not hold. Void: what is wanted
    // from it is the throw, which leaves the block before the store this
    // precedes, so the store is skipped on the violating path.
    //
    // It sits at the WRITE and never at the read. A pin's whole performance
    // model is that the read spends the claim unconditionally — a per-read
    // guard would be the deoptimization the pin exists to avoid — so the
    // claim is checked where it can be CONTRADICTED, which is the store, the
    // call site, and the boxed wrapper. A store the compiler already proved
    // (an f64-typed IL value, the closure parameter proof, the env-slot
    // fixpoint) emits none: the proof is the licence, and re-checking it
    // would tax the programs that need no barrier at all.
    PinGuard,
    // `census.record <value>, <site kind>, <key_const_index>`: hands one
    // observation to the pin census (src/runtime/pin_census.h, stage C1). Void,
    // and it NEVER THROWS — it is an instrument, and an instrument that can
    // change control flow is one whose readings are about itself.
    //
    // Emitted only under `--census`, and only where lowering has no static
    // answer for the claim a manifest would make: the mirror image of
    // `PinGuard`, which is emitted only where a manifest HAS made one. A build
    // carrying census records is never a build anything is measured on; the
    // artefact is the manifest, and the manifest is then fed to an ordinary
    // `--pins` build.
    CensusRecord,
    // `class D extends B`: links D.prototype's proto to B.prototype and
    // D's static properties to B's. One op because both links have to
    // be made together, before any method is stored.
    ClassExtend, // class.extend derived, base
    // ---- private class elements (ECMA-262 6.2.12) ----------------------
    // A private name is not a property key, so none of the ops above can
    // express one: no shape carries it, no enumeration may see it, and the
    // brand check that guards every access has no equivalent on the property
    // path. What a private name IS here is a TABLE — one per name per class
    // EVALUATION, minted by `private.new` — mapping each object that carries
    // the element to its value. Two evaluations of one class expression mint
    // two tables, which is exactly why an instance of the first fails the
    // second's brand check.
    //
    // The table is reached through an environment slot of the record the class
    // evaluation created, so a method body resolves `#x` by the same
    // (depth, index) walk a captured variable takes, and a nested class's `#x`
    // shadows an outer one's for free.
    PrivateNew,  // a: dynamic = private.new
    // `#x in o` (13.10.1). Never throws: an object without the element is
    // exactly what the operator exists to report.
    PrivateHas,  // a: bool = private.has table, obj
    // PrivateGet (6.2.12.2) minus the accessor step, which lowering resolves
    // at compile time — it knows whether the name is a field, a method or an
    // accessor pair. An object with no such element is a TypeError naming the
    // private name, which is what `key_const_index` carries.
    PrivateGet,  // a: dynamic = private.get table, obj, <key_const_index>
    // PrivateFieldAdd / PrivateMethodOrAccessorAdd (6.2.12.4): installs the
    // element, which is what makes every later access brand-check. The one op
    // with no brand check of its own, because it is what establishes the brand.
    PrivateAdd,  // private.add table, obj, value
    // PrivateSet (6.2.12.3) for a field: the element must already be there,
    // and a receiver that never got one is the same TypeError a get gives.
    PrivateSet,  // private.set table, obj, value, <key_const_index>
    // The three ways a private access is well formed and still a TypeError:
    // writing a method, reading a set-only accessor, writing a get-only one.
    // Which one is a compile-time fact, so this carries the code rather than
    // re-deriving it; the brand check has already been emitted before it.
    PrivateMisuse,  // a: dynamic = private.misuse <key_const_index>, <code>
    // The iterator protocol. One iteration is a CURSOR — opened once, stepped,
    // read, and closed if it is abandoned — rather than an index and a length,
    // because a Map, a Set and a user-defined iterable have no length to
    // compare against and no index to read at. The array/string/typed-array
    // walk survives as a kind INSIDE the record, so the common case still costs
    // no allocation and no call into user code.
    IterOpen,    // a: dynamic = iter.open b        (GetIterator, 7.4.2)
    AsyncIterOpen, // a: dynamic = async_iter.open b
    AsyncIterNext, // a: dynamic = async_iter.next %record
    AsyncIterClose, // async_iter.close %record, <suppress>
    // Advances the cursor, stashes what it produced in the record, and
    // answers whether there was anything. Two ops rather than one because
    // an SSA instruction has one result and a step has two answers.
    IterStep,    // a: bool = iter.step %record     (IteratorStep, 7.4.6)
    IterValue,   // a: dynamic = iter.value %record
    // IteratorClose (7.4.9): the iterator's `return` method, for a for-of
    // left by `break`, `return` or `throw`. `immI32` is 1 when a throw is
    // already in flight, which is when 7.4.9 step 6 discards an error the
    // `return` method raises rather than letting it replace the original.
    IterClose,   // iter.close %record, <suppress>
    // Everything the cursor has left, as a fresh array — a rest element's
    // value. Drains the same record the elements before it were stepped from.
    IterRest,    // a = iter.rest %record
    // One RESUMPTION forwarded to a delegated iterator: ECMA-262 27.5.3.7
    // steps 5.a, 5.b and 5.c, without the loop around them. The loop is
    // compiled code — it has a suspension in it, and a suspension is a return
    // from the resume function — so what is left for one instruction is the
    // part that is not control flow: WHICH method of the inner iterator this
    // resumption calls, and what happens when it has none.
    //
    // `b` is the record, `c` the resumption kind as a number (the runtime's
    // GeneratorResumeMode), and `d` the value it carried. The result is the
    // inner iterator's RESULT OBJECT — forwarded by identity, which is what
    // 27.5.3.8 GeneratorYield does with it — or `undefined` for the one case
    // that produces no object at all: a `return` resumption to an iterator
    // with no `return` method, which 5.c.iii passes straight through.
    IterDelegate,  // a: dynamic = iter.delegate %record, %mode, %sent
    // The source of a destructuring, checked once before any element is read.
    // `immI32` names which pattern asked, so the diagnostic can say `array
    // destructuring` rather than `for-of`, and the check is what lets every
    // element read below it assume a walkable value.
    PatternCheck,  // a = pattern.check b, <kind>
    // The three container-building ops a spread needs: one element, then all of
    // an iterable's, then all of an object's own enumerable properties.
    // Appending rather than indexing is the point — a literal with a spread in
    // it has no length until it is built.
    ArrayAppend,  // array.append arr, v
    ArrayAppendHole, // array.append.hole arr
    ArraySpread,  // array.spread arr, iterable
    ObjectSpread, // object.spread obj, source
    // `{ a, ...others }`: a fresh object of `source`'s own enumerable
    // properties minus the ones the pattern already named, which arrive as
    // an array of keys because a computed key is not known until it runs.
    ObjectRest,   // a = object.rest source, excludedKeys
    // A call whose argument list contains a spread, so its length is a
    // runtime fact: the arguments are built into an array and the callee is
    // entered through the uniform convention over it.
    DynamicCallSpread,  // a = call.dynamic.spread callee, thisArg, args
    MethodCall,         // a = method.call thisArg, <key_const_index>, <ic_site_index>, args...
    MethodCallSpread,   // a = method.call.spread thisArg, <key_const_index>, <ic_site_index>, argsArr
    SuperCall,          // a = call.super base, thisArg, args...
    SuperCallSpread,    // a = call.super.spread base, thisArg, args
    // 13.2.8.4 GetTemplateObject. The pair is a CACHE, not one instruction
    // split in two: the specification keeps a template object per SITE, so
    // `f()===f()` holds for a tag called twice from one `` t`x` ``, and the
    // arrays are built only on the call that misses.
    //
    //   a = template.cached <site>            ; the cell, undefined when cold
    //   a = template.object cooked, raw, <site>  ; builds, freezes, fills the cell
    TemplateCached,     // a = template.cached <site>
    TemplateObject,     // a = template.object cookedArray, rawArray, <site>
    ConstructSpread,    // a = new.spread callee, args
    CreateArray,  // a = create.array <length>
    CreateFunction,// a = create.func <funcIndex>, env
    FunctionRef,   // a = func.ref <funcIndex>
    EnvCreate,  // a = env.create parent, <slots>
    EnvGet,     // a = env.get env, <depth>, <index>
    EnvSet,     // env.set env, <depth>, <index>, v
    // The temporal dead zone, in two instructions. A scope entry writes the
    // uninitialized-binding marker into the slot of every `let`, `const` and
    // `class` it declares, and every READ of such a slot goes through the
    // checked form, which is 9.1.1.1.6 GetBindingValue's ReferenceError.
    //
    // Two ops rather than a flag on env.get/env.set, for the reason `print.err`
    // is not a flag on `print`: the canonical dump is what a reader bisects
    // with, and whether a read can throw is exactly the kind of fact a field
    // can silently omit. `env.get.tdz` also carries the NAME, because which
    // binding was read too early is the whole content of the diagnostic.
    //
    // A `var`, a parameter, a hoisted `function` and the synthetic `this` and
    // `arguments` slots never take these: none of them is ever uninitialized,
    // and giving them a check would be a dead zone the language does not have.
    EnvInitTdz, // env.init.tdz env, <depth>, <index>
    EnvGetTdz,  // a = env.get.tdz env, <depth>, <index>, <key_const_index>
    // The MODULE scope's environment record, which is a singleton: the top
    // level runs exactly once, so there is exactly one activation of that scope
    // and no reason to thread it through every calling convention. `main`
    // publishes it; a module function that needs a module-level binding loads
    // it at entry and chains its own record to it.
    ModuleEnvSet,  // module.env.set %env
    ModuleEnvGet,  // a: dynamic = module.env.get
    Print,      // print a, ...            (console.log / info / debug)
    // Same formatter, other stream. Its own op rather than a flag on `Print`
    // because the canonical dump is what a reader bisects with, and a stream
    // carried in a field is a stream the dump can silently omit.
    PrintErr,   // print.err a, ...        (console.warn / error)
    PrintSpread, // print.spread arr
    PrintSpreadErr, // print.spread.err arr
    // keep.alive a, ...: reads its operands and does nothing, so each stays
    // live, and its object reachable, up to here. A native call puts one
    // after the call and its result's conversion for every handle whose data
    // pointer the native was given: the handle's destructor must not run
    // while that pointer, or anything the native derived from it, is in use.
    KeepAlive,
};

const char* opName(Op op);
bool isTerminator(Op op);

// The function selector `math.unary` carries in `immI32`. Membership is the
// soundness line: a function goes on this list only if IEEE 754 defines its
// result exactly, so backend intrinsic and runtime libm agree bit for bit.
enum class MathUnaryFn : uint32_t {
    Sqrt = 0, Abs = 1, Floor = 2, Ceil = 3, Trunc = 4, Sin = 5, Cos = 6
};

}  // namespace bronze::il
