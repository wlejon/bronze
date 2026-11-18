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
    // At type i32 (speculated code only) the add, sub and mul below are
    // WRAPPING two's-complement and stand behind an `*.would_ovf` guard.
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
    // is ordinary IL that would have existed anyway.
    IsNumber,   // a: bool = is.number b     (b: dynamic)
    // The DENSE-ARRAY TEST: does this receiver answer every constant-index read
    // up to `immI32` out of its own element block, with no user code and no
    // allocation? It is to a constant-index read what `is.number` is to a
    // coercion, so one test covers a run of reads. It compiles to the backend's
    // own receiver proof (llvm_recv_proof.h), so it claims nothing the reads it
    // stands in front of were not already going to assume
    // (lower_pattern.cpp).
    // With kDenseNumbers in immI32 it also asks the array's NUMBERS bit
    // (runtime/array.h): every element below the length is a Number in its
    // double form, so a read in bounds needs no hole or number test after it.
    // A clear bit is claimed on the spot (bronze_array_claim_numbers) before
    // the test answers no. Reads `Elements` too: a store may clear the bit.
    IsDenseArray,  // a: bool = is.dense_array b, <immI32: max index | kDenseNumbers>
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
    SuperGet,   // a = super.get lookupStart, <key_const_index>, thisArg
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
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
    Construct,  // a = new callee, args...
    CreateObject, // a = create.object
    // The three ops of a COROUTINE BODY: a generator, async function or async
    // generator (src/lower/lower_generator.cpp). The function a program calls
    // is a stub that starts the body, which is a separate IL function
    // (`Function::coroKind`) compiled onto a brass coroutine.
    //
    // `coro.start` creates the body's frame with the operands as its
    // arguments (calleeIndex names the body, immI32 its CoroKind) and hands it
    // to the runtime, which answers what the call returns: the generator
    // object, or the async function's promise. It runs the body — to its
    // first suspension — and so reaches user code.
    CoroStart,   // a = coro.start <kind> @body(args...)
    // A suspension of the body (immI32 a SuspendKind): the operand goes to
    // the resumer and the result is the value the next resumption sent.
    CoroSuspend, // a = coro.suspend <kind> b
    // How the latest resumption asked the body to continue: next, throw or
    // return (CoroResumeMode; bronze_abi.h). Read right after a suspension.
    CoroMode,    // a: i32 = coro.mode
    DynamicImport,      // a = dynamic_import specifier, <url_const_index of the importer>
    // A MODULE NAMESPACE EXOTIC OBJECT (ECMA-262 10.4.6), built from the object
    // of getters the operand holds. Its own op for the reason
    // `create.generator_object` is one: what it produces is not an object
    // literal with a different attribute set, it is a receiver KIND whose
    // own-key order, [[Set]] and [[GetOwnProperty]] are each its own.
    ModuleNamespace, // a = module.namespace b
    // The keys a `for-in` will visit, as one array built before the first
    // iteration: own AND inherited enumerable string keys, each once.
    // Snapshotting is what lets the loop itself be for-of's index walk over the
    // result, and it is a legal answer to the spec's open question about keys
    // ADDED during enumeration; a key deleted is `forin.same`'s question.
    ForInKeys,  // a = forin.keys b
    // A key deleted before the loop reaches it is not visited (14.7.5.9), which
    // the snapshot alone cannot know (runtime/rt_enumerate.cpp). The thread
    // counts the ways a key can leave an object (bronze_abi_tls.h
    // for_in_epoch). `forin.guard`, right after the `forin.keys` that is its
    // operand, is the count that snapshot was taken at (a double, -1 for a
    // receiver whose every key is asked); `forin.same` is whether the count
    // is still that, a TLS load and an integer compare. The loop visits a key
    // without asking while it is.
    ForInGuard,  // a: f64 = forin.guard b
    ForInSame,   // a: bool = forin.same b
    // Whether the loop over b still visits key c, asked when the counts
    // differ: V8's ForInFilter (rt_enumerate.cpp, bronze_for_in_live). Calls
    // a proxy's traps.
    ForInLive,   // a: bool = forin.live b, c
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
    AsyncIterClose, // a: dynamic = async_iter.close %record, <suppress>
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
    // compiled code — it has a suspension in it — so what is left for one
    // instruction is the
    // part that is not control flow: WHICH method of the inner iterator this
    // resumption calls, and what happens when it has none.
    //
    // `b` is the record, `c` the resumption kind as a number (a boxed
    // CoroResumeMode), and `d` the value it carried. The result is the
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
    // `super(...)`: `activeFn` is the derived constructor; the base is its
    // [[Prototype]] when the call runs (13.3.7.2 GetSuperConstructor).
    SuperCall,          // a = call.super activeFn, thisArg, args...
    SuperCallSpread,    // a = call.super.spread activeFn, thisArg, args
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
    // a: bool = is.typed_array b, <immI32: types::TypedArrayElem number>.
    // Whether `b` is a TypedArray of exactly that element kind. Both facts
    // are fixed when the view is made — no operation changes an object's
    // kind — so a test that held once holds for as long as the same value
    // is in hand, across calls, detaches and collections alike; what such a
    // call CAN change, the view's length and where its bytes are, the typed
    // element accesses read afresh at every access. That is the licence for
    // the element flag kElemFlagWordViewReceiver (il_pin.h) on the true edge
    // (lower_pattern.cpp). Header loads only: cannot throw, cannot collect.
    //
    // With kTypedArrayPristineLength set in `immI32` it also requires what the
    // runtime's own `length` read requires to answer from the view header
    // (rtTypedArrayChainPristine): the view has its kind's intrinsic instance
    // shape and the family was verified pristine at the current
    // prototype-mutation epoch. Unlike the kind, that holds only until code
    // runs that could define a property or patch a prototype.
    IsTypedArray,
    // object.proto obj, v. An object literal's `__proto__: v` (13.2.5.5 step
    // 6, B.3.1): the fresh object's [[SetPrototypeOf]](v) when v is an object
    // or null, nothing otherwise. Not a property write: the literal's other
    // properties are DEFINES (prop.set's kWriteDefine), which would make an
    // own `__proto__` of this one. Only the colon form with a written name
    // reaches here; a computed, shorthand or method `__proto__` is a define.
    ObjectLitProto,
    // `feedback.record [<value>], <key_const_index>, <immI32: site info>`:
    // hands one observation to the type-feedback recorder
    // (src/runtime/type_feedback.h, docs/type-feedback.md). The key is the
    // function's feedback key and the info word packs the site's kind and
    // ordinal (abi/bronze_feedback_site.h); an `entry` site has no operand.
    //
    // Emitted only under `--feedback-out`, by lower/lower_feedback.cpp, as
    // the LAST thing lowering does — so the sites are the final IL's own and
    // no pass ever sees one. Void; it never throws and never collects: the
    // recorder classifies the value's tag and names its shape or callee from
    // immortal shape and string memory, allocating nothing on the JS heap. An
    // instrument that could change control flow would be measuring itself.
    FeedbackRecord,
    // a: bool = is.plain_array b. Whether `b` is an Array exotic object with
    // the bare array shape: no own named property, no integrity level, the
    // realm's Array.prototype as its [[Prototype]] (a subclass instance, a
    // replaced prototype and every integrity level move an array off it,
    // runtime/array.h). Such an array's [[Get]] of an Array.prototype method
    // answers the intrinsic function, since Array.prototype refuses every
    // change (rt_prop_write.cpp). The guard of a call site's direct edge into
    // the program's copy of a self-hosted builtin (lower/selfhost_merge.cpp).
    // NOT a fact about the value: a write of a named property gives the
    // array a new shape, so it is never merged, hoisted or carried across anything.
    // Header loads only: cannot throw, cannot collect.
    IsPlainArray,

    // ---- speculation: leaving a speculated function ---------------------
    // A function S speculated from a generic function G (il.h
    // `Function::speculatedFrom`) tests its assumptions with ORDINARY
    // branches. The edge that sees an assumption fail goes to a cold exit
    // block, which may do exit-only work (box a value, re-create an object
    // whose allocation S removed) and ends in `leave`, handing the frame's
    // state to G at a marked point. There is no guard instruction with a
    // hidden edge: what S knows, it knows from branch edges (opt/core
    // edge_facts.h), and every pass reads one effect model (il/effects.h).
    //
    // `resume.point <id>, %s0, %s1, ...` marks the place in G a `leave` may
    // resume at, in front of the instruction resumption continues with.
    // `immI32` is the id, unique within G (handed out dense from 0; one that
    // nothing names may be deleted, so density is not an invariant).
    // The operands are the STATE: every value live there that G's code from
    // this point on needs, in a fixed order a `leave` matches positionally.
    // No result, no effects of its own, but it is an ENTRY into G: no
    // instruction, pure ones included, may be moved across it while a
    // `leave` names it (Effects::barrier), because a resumption arrives here
    // with only the state in hand. Never in a speculated function.
    // `keyIndex` is the point's KIND: 0, or kResumeOptimistic for a point only
    // an optimistic function S+ (il.h Function::fallback) leaves for, which
    // counts toward retiring S+ to S rather than S to G (docs/deopt-stats.md).
    // An optimistic point stands right after the ordinary point of the same
    // place when both exist, with the same state (printed `optimistic`).
    ResumePoint,
    // `leave @G, <id>, %s0, %s1, ...`: terminator. Leaves S for G's
    // `resume.point <id>` with the operands as its state, which match that
    // point's operands in count and in type, one for one. `calleeIndex` is
    // G's index in Module::functions and must be S's `speculatedFrom`.
    // `keyIndex` repeats the point's kind (kResumeOptimistic: only in an
    // optimistic function), so a pass reading an exit knows it without G.
    // No successors: what runs next is G's, and S never sees it again.
    Leave,

    // ---- speculation: the low-level forms S is written in -------------
    // Each is one machine-level step with no fallback. The UNCHECKED ones are
    // legal only where a dominating branch edge established their
    // precondition (named on each); the verifier cannot check a dominance fact
    // cheaply and does not try, so a precondition broken by a pass is a wrong
    // read and not a diagnostic. Those carry Effects::guarded: no pass may
    // hoist one above the branch that licensed it.
    //
    // a: ptr = load.shape %obj. The shape word at BRONZE_ABI_OBJ_SHAPE_OFFSET
    // (every object kind carries one there). %obj is dynamic and must be an
    // object (`is.object` true edge, or a fact implying it). Reads `Shapes`.
    LoadShape,
    // a: ptr = shape.cell <n>. The shape the module's layout cell `n` holds
    // (Module::shapeCells, `immI32` = n): a word filled once when the module
    // is loaded, from the harvest's name for it, and never written again —
    // so the read is pure. A cell the loader could not resolve holds a word
    // no object carries, so every compare against it fails and S leaves.
    ShapeCell,
    // a: bool = ptr.eq %a, %b. Both ptr. `ptr.eq (load.shape x), (shape.cell n)`
    // is the shape test edge_facts recognises.
    PtrEq,
    // a: bool = shape.has_own %shape, <slot, mode>, "k". Whether an object
    // at shape %shape (ptr, a `load.shape`) has the string key `keyIndex` as
    // an own data property in slot `immI32 >> kHasOwnSlotShift`, held as the
    // mode bits ask: kHasOwnWrite, writable on a shape that is no prototype's
    // and (without kHasOwnNumber) boxed, so any Value may be stored;
    // kHasOwnNumber alone, a double slot (runtime/slot_repr.h), so the read
    // is a Number; both, writable in either representation, for a store of a
    // Number. So on its true edge `slot.load` (and with kHasOwnWrite,
    // `slot.store` of what the mode admits) of that slot is the [[Get]] (the
    // [[Set]]) of the key, whatever the shape is: the
    // test of a site whose receivers come in more shapes than a chain of
    // `ptr.eq` tests covers. Reads the shape's slot-key table (runtime
    // shape_slot_keys.cpp), built on first use; a shape's layout never
    // changes, so the answer for one shape never does, and the read is pure.
    // The prototype mark is monotone the other way (a shape becomes one, never
    // stops), which only turns a true answer false.
    ShapeHasOwn,
    // a: bool = shape.lacks %obj, %shape, "k". Whether [[Get]] of the string
    // key `keyIndex` on %obj (an object, by an `is.object` guard), whose
    // shape is %shape (its `load.shape`), finds nothing: the shape has no
    // such own property and no link of its prototype chain has one, as of
    // this thread's prototype epoch. So on its true edge the read is
    // `undefined`, whatever the shape is: the absent arm of a site whose
    // receivers come in more shapes than member cells cover. Asks the
    // shape's absent cache (runtime shape_slot_keys.cpp, beside the slot-key
    // table), which the runtime fills on a miss for a plain, non-dictionary
    // receiver it finds the key absent from (accessor_cell.cpp
    // bronze_shape_lacks); a true answer holds until the epoch moves. Reads
    // `Protos`; runs no user code and allocates nothing on the JS heap.
    ShapeLacks,
    // a: bool = shape.in %shape, %c1, ..., %cn. Whether the ptr %shape (a
    // `load.shape`) equals one of the ptrs %c1..%cn (n >= 2, each a
    // `shape.cell`): the test of a receiver whose shape is one of a set. Its
    // true edge proves the set (edge_facts.h), its false edge that the shape
    // is none of them. Pure.
    ShapeIn,
    // a: bool = is.object %v. The tag test: %v (dynamic) is a heap object
    // reference (BRONZE_ABI_TAG_OBJECT), so `load.shape` is legal on it.
    // Functions and arrays are objects. Total, pure.
    IsObject,
    // a: dynamic = slot.load %obj, <slot>. Named-property slot `immI32`: below
    // BRONZE_ABI_OBJ_OVERFLOW_SLOT the inline Value at that index, at or above
    // it Value (slot - OVERFLOW_SLOT) of the overflow block; the backend picks
    // the form from the constant. Unchecked: %obj's shape is known (a shape
    // fact) and that shape places a data property at `slot`. Reads `Slots`.
    // `boxType` F64 (printed `number`): every shape the guards in front of it
    // admit keeps the slot a DOUBLE one (`%k`, opt/core/shape_key.h), so the
    // value is a Number, read as the double it is.
    SlotLoad,
    // slot.store %obj, <slot>, %v. The store form of `slot.load`, write barrier
    // included. Writes an EXISTING slot: it never transitions the shape, so it
    // writes `Slots` and not `Shapes` (a transition is still a generic
    // `prop.set`, or a later dedicated op). Into a double slot only a Number
    // may be stored: the speculation tests %v first (`is.number`).
    SlotStore,
    // a: bool = is.i32 %v. %v (dynamic) is a Number whose value is an int32:
    // a double that is integral, in range and not -0 (a Number has one box,
    // the double's bits; the backend also accepts the unused Int32 tag).
    // Total, pure. The Number test is the existing `is.number` (bits <=
    // BRONZE_ABI_NUMBER_MAX_BITS), so there is no separate `is.f64`.
    IsI32,
    // a: i32 = unbox.i32.unchecked %v. The int32 a dynamic %v holds.
    // Unchecked: legal only on `is.i32`'s true edge. Unlike `unbox.i32`
    // (checked, an ordinary conversion) it never calls. Pure: on a value of
    // another tag it computes a wrong number and touches no memory.
    UnboxI32,
    // a: f64 = unbox.f64.unchecked %v. The double a dynamic %v holds.
    // Unchecked: legal on `is.number`'s true edge (and so on `is.i32`'s). Not
    // ToNumber: `unbox.f64` stays the checked conversion.
    UnboxF64,
    // a: f64 = i32.to_f64 %i. Exact; pure.
    I32ToF64,
    // Overflow-checked int32 arithmetic is a GUARD CONDITION in front of the
    // plain typed op: `c: bool = add.would_ovf %a, %b; br %c, bExit, bFast`,
    // and on the fast edge `s: i32 = add %a, %b`. `add`/`sub`/`mul` typed i32
    // wrap two's-complement; they are only correct as JS arithmetic where the
    // matching *.would_ovf answered false. One result per instruction is the
    // IL's rule, and this keeps it without a flags pseudo-value.
    //
    // a: bool = add.would_ovf %a, %b   (both i32): a + b is outside int32.
    AddWouldOvf,
    // a: bool = sub.would_ovf %a, %b   (both i32): a - b is outside int32.
    SubWouldOvf,
    // a: bool = mul.would_ovf %a, %b   (both i32): a * b is not EXACTLY an
    // int32 Number — outside the range, OR -0 (a zero product with a negative
    // operand), which an i32 cannot represent.
    MulWouldOvf,
    // a: bool = fn.code_is %callee, @f. Whether dynamic %callee is a function
    // object whose code is module function `calleeIndex` — the identity test
    // that licenses a direct `call @f`. Total (it includes the object and
    // function-kind test): false for anything else. A function's code never
    // changes, so the fact never dies. Pure.
    FnCodeIs,
    // a: dynamic = fn.env %callee. The environment record the closure %callee
    // captured, for a direct call to a `needsEnv` function. Unchecked: legal
    // only on a `fn.code_is` true edge. The captured record is fixed when the
    // closure is made, so the read depends on no location.
    FnEnv,
    // a: i32 = array.length %arr. The dense array's length word. Unchecked:
    // %arr is an Array (`is.dense_array` or `is.plain_array` true edge).
    // Reads `ArrayLen`.
    ArrayLength,
    // a: dynamic = elem.load %arr, %i. Element %i (i32) of the array's element
    // block, as stored: a hole reads as the HOLE value, never `undefined`, and
    // no prototype is asked — so pair it with `is.hole`. Unchecked: %arr is an
    // Array and %i is in bounds (`in.bounds %i, %len` with %len its
    // `array.length`). Reads `Elements`. immI32 kElemLoadNumber: the array
    // passed `is.dense_array` with kDenseNumbers, so the element is a Number
    // in its double form (never the hole).
    ElemLoad,
    // elem.store %arr, %i, %v. Stores %v (dynamic, never the hole) into the
    // element block at %i (i32), write barrier included. Unchecked as
    // `elem.load` is; it never grows the array, so it writes `Elements` and
    // not `ArrayLen`, and `ElemKinds` unless immI32 has kStoreNumber.
    ElemStore,
    // a: bool = in.bounds %i, %len. Both i32; the UNSIGNED compare
    // `(uint32)i < (uint32)len`, so a negative index is out. Pure.
    // WIDENED (`immI32` = span > 0, opt/redundancy/bounds.cpp): i >= 0 and
    // i + span < len, exactly (a negative len is no length).
    InBounds,
    // a: bool = is.hole %v. %v (dynamic) is the array-hole value
    // (BRONZE_ABI_HOLE_BITS), which only `elem.load` can produce. Pure.
    IsHole,
    // a: bool = is.uninit %v. %v (dynamic) is the uninitialized-binding marker
    // (BRONZE_ABI_UNINITIALIZED_BITS), which only `env.get` of a lexical slot
    // can produce: speculation's form of `env.get.tdz` is that read with this
    // test guarded false (opt/speculate/tdz.cpp). Pure.
    IsUninit,

    // ---- speculation: finishing an inlined call generically -------------
    // `a = resume.call @G, <id>, %s0, %s1, ...`: runs G's resume twin from
    // G's `resume.point <id>` with the operands as its state, to G's return,
    // and yields what G returns. NOT a terminator: control comes back. It is
    // what a `leave` of an INLINED speculated body becomes (opt/inline): the
    // inlined callee's guard failed, so the rest of that call runs in the
    // callee's generic function, and the caller then leaves for its own
    // generic function after the call with this result in the call's place.
    // `calleeIndex` is G's index, `immI32` the id, `keyIndex` the point's kind;
    // the state matches that point's operands in count and type exactly as a
    // `leave`'s does. The
    // result has G's return type, and there is none when G returns void.
    // Only in speculated functions. Effects: everything (it runs G's code).
    ResumeCall,

    // ---- speculation: kinds, typed arrays and intrinsics ----------------
    // a: bool = is.array %v. %v (dynamic) is an Array exotic object. An
    // object's kind is fixed when it is made, so the fact never ends and the
    // test reads nothing that changes. Total, pure. Licenses `array.length`.
    IsArray,
    // a: i32 = typed.length %ta. The view's length in elements, as its header
    // holds it now (zero once its buffer is detached). Unchecked: %ta is a
    // typed array (`is.typed_array` true edge). Reads `Views`.
    TypedLength,
    // a: f64|i32 = typed.load %ta, %i, <kind>. Element %i (i32) of the view,
    // whose element kind is `immI32` (types::TypedArrayElem): f64 for the
    // float kinds and Uint32, i32 for the other integer kinds. Unchecked: %ta
    // is a view of that kind and %i is below its `typed.length`. Reads
    // `Views` (where the bytes are) and `TypedElements`.
    TypedLoad,
    // typed.store %ta, %i, %v, <kind>. Stores %v (f64 for the float kinds,
    // i32 for the integer kinds: the low bits, or the clamped byte for
    // Uint8Clamped from an f64) as element %i, converted as [[Set]] on a
    // Number does. Unchecked as `typed.load` is. Reads `Views`, writes
    // `TypedElements`.
    TypedStore,
    // a: bool = fn.native_is %f, <intrinsic>. Whether dynamic %f is a function
    // object whose code is the runtime's native for `immI32` (il::Intrinsic):
    // the identity that makes `Math.floor(x)` the floor of x. A function's
    // code never changes, so the fact never dies. Total, pure.
    FnNativeIs,
    // a = math.<intrinsic> %x[, %y]. The intrinsic `immI32` computed on
    // numbers: f64 operands and result, except `imul` (i32, i32 -> i32).
    // Exactly the Math function on Numbers. Pure.
    MathFn,

    // ---- literals: an object or array made with its final layout ---------
    // `a = object.literal <lit>, %v0, ..., %v(n-1)`: 13.2.5.5 for a literal of
    // n spelled data properties with distinct named keys and nothing else (no
    // accessor, spread, computed key, `__proto__:` or index key): a fresh
    // ordinary object whose own properties are Module::objectLiterals[lit]'s
    // keys in order, key i holding %vi, each an enumerable, writable,
    // configurable data property (CreateDataPropertyOrThrow on a fresh
    // object, which never consults the prototype chain). `immI32` = lit. The
    // values are evaluated before it (lower_object.cpp). An allocation.
    ObjectLiteral,
    // `a = array.literal %v0, ..., %v(n-1)`: 13.2.4.1 for an array literal
    // with no elision and no spread: a fresh Array of length n, element i
    // %vi (defined, never [[Set]]). An allocation.
    ArrayLiteral,

    // ---- speculation: construction, transitions, prototype facts --------
    // a: bool = fn.constructs %f. Whether `new` may make %f's instance
    // inline: %f is a function object (a `fn.code_is` true edge licenses the
    // read) whose plain construction path the runtime has taken once (the
    // construct-vetted byte, bronze_abi.h), so it has an instance shape and
    // its instances are ordinary objects. The byte never clears. Reads
    // `FnFields`. Unchecked as `fn.env` is.
    FnConstructs,
    // a: ptr = fn.instance_shape %f. The shape `new %f` gives its instance:
    // the root over %f.prototype (rt_object.cpp rtEnsureFunctionPrototype;
    // replaced when `prototype` is). Unchecked: `fn.constructs` true edge.
    // Reads `FnFields`.
    FnInstanceShape,
    // a = create.this %f, %s. A fresh ordinary object of shape %s (which
    // must be `fn.instance_shape %f`), every slot undefined: the instance
    // OrdinaryCreateFromConstructor makes for `new %f` (bronze_construct's
    // plain path). Unchecked as `fn.instance_shape` is. An allocation.
    CreateThis,
    // shape.store %obj, %s. Writes %s as %obj's shape word: the second half
    // of a property ADD whose slot store came first (prop_write.cpp). Writes
    // `Shapes`. Unchecked: %obj is a plain object whose shape is the add's
    // from-shape, and %s is that shape's child for the key, whose slot the
    // store filled.
    ShapeStore,
    // a: bool = proto.cell <c>. Whether transition cell c
    // (Module::protoCells) is stamped with this thread's prototype epoch:
    // the add it speaks for is, until the next bump, a slot store and a
    // shape store. `immI32` = c. Reads `Protos`.
    ProtoCell,
    // proto.publish <c>. Asks the runtime to check cell c's add and stamp
    // the cell with the current epoch (or leave it as it is when the chain
    // or the shapes answer anything else). Runs no user code and allocates
    // nothing on the JS heap. Writes `Protos`. Only in an exit block, before
    // its leave.
    ProtoPublish,
    // a: ptr = member.cell <c>. Word 0 of member cell c (Module::memberCells):
    // the receiver shape the runtime last found to resolve the cell's key,
    // above the receiver, to the cell's function (a method in a data
    // property, or an accessor's getter or setter), to nothing (an absent
    // cell) or to a data property at the cell's place (a data cell) at this
    // thread's prototype epoch, or a word no shape has. The runtime empties every
    // such cell at each bump of the epoch. `immI32` = c. Reads `Protos`.
    MemberCell,
    // member.publish %recv, <c>. Asks the runtime to check what %recv's
    // chain resolves the cell's key to and, when it is the cell's function
    // and no other cell of its group holds %recv's shape, to put that shape
    // in the cell. Runs no user code and allocates nothing on the JS heap.
    // Writes `Protos`. Only in an exit block, before its leave.
    MemberPublish,
    // a: dynamic = shape.proto %shape. The [[Prototype]] of every object at
    // the shape %shape (ptr, a `load.shape`): the Value its root keeps.
    // Unchecked: legal only where a member cell's test licensed the walk
    // (a DATA cell, il.h Module::MemberKind), so the result is a plain
    // object at the depth the cell names, or the link to one; `load.shape`
    // and `slot.load` on it are then legal too. Reads `Protos`.
    ShapeProto,
    // a = construct.result %r, %this: %r when it is an object, else %this —
    // the value of `new` given what a base constructor returned (10.2.2
    // [[Construct]] steps 10-12). Pure.
    ConstructResult,
    // a: dynamic = concat.n %p0, ..., %pk (k >= 1). A template literal's
    // pieces joined in one step: every operand is a String, or a Number
    // when it is a `to.string` carrying kToStrKeepNumber (whose only use is
    // this op), converted here as ToString converts it. Nothing it does is
    // observable but the fresh String it returns, made at its exact length.
    // Allocates; throws only the RangeError of a too-long string.
    ConcatN,
    // a: bool = typeof.is %v, <kind>. `typeof %v === name` for the name
    // TypeOfKind `immI32` stands for (13.5.3's table): a tag test, plus for
    // "object" and "function" whether an object is callable, which is fixed
    // when it is made. What `typeof x === "lit"` (and `!==`, `==`, `!=`, either
    // way round) lowers to (lower_expr_binary.cpp). Total, pure.
    TypeOfIs,
    // a: dynamic = array.push b, v. `b.push(v)` (23.1.3.23) where a dominating
    // `is.plain_array b` held: an Array at the bare array shape, whose `push`
    // can only be %Array.prototype.push% and whose chain (the frozen
    // Array.prototype and Object.prototype) holds no index a Set could reach.
    // Appends `v`, answers the new length. Writes that one array's length and
    // elements and nothing else (no user code runs), so a loop filling a fresh
    // array keeps every other array's facts (opt/redundancy/location.cpp).
    // Grows the element block (allocates); throws the RangeError of a length
    // past 2^32 - 1. Speculation's form of a `push` on an array the function
    // made (speculate/array_push.cpp). immI32 kStoreNumber as `elem.store`'s.
    ArrayPush,
    // a = apply.args recv, thisArg, count, v0, ..., vN-1 <key "apply">,
    // <ic_site_index>: `recv.apply(thisArg, list)` where the list is the
    // first `count` (a Number, at most N) of the values, which is how a
    // counted function forwards its arguments (il.h argumentsIsCount,
    // kForwardedArguments). Exactly that method call: a function at the
    // bare function shape whose site latched %Function.prototype.apply% is
    // entered with the values as its arguments and no Array made; any other
    // receiver gets the list made and the call made through the site's
    // method cache, a method site like `method.call`'s. Calls anything.
    ApplyArgs,
    // a: bool = is.collection v, <class>. Whether `v` is an object at the
    // instance shape `new Map()` (`Set`, `WeakMap`, `WeakSet`, and a RegExp
    // literal or `new RegExp`: immI32 a BRONZE_ABI_COLLECTION_ class) makes
    // its instances at: no own property and the class's prototype, which
    // lockdown froze, so `v.get` / `v.has` / `v.exec` read that prototype's
    // natives. NOT a fact about the value (a property
    // added moves it off the shape), like is.plain_array. Header loads only.
    IsCollection,
    // a: dynamic = collection.op recv, key, <op>. `recv.get(key)` /
    // `recv.has(key)` (immI32 a BRONZE_ABI_COLLECTION_ op) where a
    // dominating is.collection held for the op's class: the native's call,
    // made without the method read (runtime bronze_collection_op). Reads the
    // table (Loc::Heap), may rebuild its index (allocates), runs no user
    // code; throws the native's TypeError for an object with the shape and
    // not the internal slots. Speculation's form of a harvested
    // `get` / `has` (speculate/collection_call.cpp).
    CollectionOp,
};

// typeof.is's immI32: which answer of `typeof` it tests for. kTypeOfNone
// is a literal `typeof` never answers, which is false for every value.
enum TypeOfKind : int32_t {
    kTypeOfUndefined = 0,
    kTypeOfObject = 1,
    kTypeOfBoolean = 2,
    kTypeOfNumber = 3,
    kTypeOfString = 4,
    kTypeOfSymbol = 5,
    kTypeOfBigInt = 6,
    kTypeOfFunction = 7,
    kTypeOfNone = 8,
};

// The flag on `to.string`'s immI32 that lets a Number through unconverted:
// the result is a String or a Number, and its one use is a `concat.n`
// operand, which converts the Number itself (lower_expr.cpp, templates).
inline constexpr int32_t kToStrKeepNumber = 1;

// The last op, for the serializer's range check.
inline constexpr Op kLastOp = Op::CollectionOp;

// The intrinsics `fn.native_is` and `math.*` name (immI32): Math functions
// whose native's code pointer the runtime exports, so a test can compare a
// callee against it (bronze_abi_functions.h). `intrinsicInfo` has each one's
// JS name, argument count and native symbol.
enum class Intrinsic : int32_t {
    MathAbs,
    MathFloor,
    MathCeil,
    MathRound,
    MathSqrt,
    MathMin,
    MathMax,
    MathImul,
    MathSin,
    MathCos,
    MathTan,
    MathAsin,
    MathAcos,
    MathAtan,
    MathAtan2,
    MathExp,
    MathLog,
};
inline constexpr int32_t kIntrinsicCount = static_cast<int32_t>(Intrinsic::MathLog) + 1;

struct IntrinsicInfo {
    const char* name;    // the Math member: "floor"
    uint8_t arity;       // the arguments the computed form takes
    const char* native;  // the function object's code: "bronze_math_floor"
};
// Info for `i`, or nullptr when `i` is no intrinsic.
const IntrinsicInfo* intrinsicInfo(int32_t i);

// The bit of `is.typed_array`'s immI32 above its element kind (see there).
inline constexpr int32_t kTypedArrayPristineLength = 1 << 8;
// The bit of `is.dense_array`'s immI32 above its max index (see there), and
// `elem.load`'s immI32 under such a test.
inline constexpr int32_t kDenseNumbers = 1 << 30;
inline constexpr int32_t kDenseIndexMask = kDenseNumbers - 1;
inline constexpr int32_t kElemLoadNumber = 1;
// `elem.store`'s and `array.push`'s immI32: the stored value is known to be a
// Number (the redundancy pass sets it from the value's definition or facts),
// so the store keeps the array's NUMBERS bit and does not write `ElemKinds`
// (effects.h).
inline constexpr int32_t kStoreNumber = 1;
// `shape.has_own`'s immI32: (slot << kHasOwnSlotShift) | mode bits.
inline constexpr int32_t kHasOwnWrite = 1;
inline constexpr int32_t kHasOwnNumber = 2;
inline constexpr int32_t kHasOwnSlotShift = 2;
// `resume.point`'s keyIndex (see there): an optimistic point.
inline constexpr uint32_t kResumeOptimistic = 1;

const char* opName(Op op);
bool isTerminator(Op op);

}  // namespace bronze::il
