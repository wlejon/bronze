#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "abi/bronze_abi.h"

namespace bronze::il {

// bronze IL: a typed, SSA, mid-level IR. Static layouts are used wherever
// analysis can PROVE them (inference-first; TS annotations are untrusted
// hints); Dynamic is the explicit fallback type for code the analysis
// cannot type — wild JS must always compile. Dynamic is the fallback and
// never the substrate: a value is boxed because nothing PROVED it, not
// because boxing is the default the proofs opt out of.
//
// Deliberately tiny today: enough structure to carry lower→codegen work and
// to pin the canonical text form. Every addition must keep print(parse(x))
// byte-stable once the text parser lands.

enum class Type : uint8_t {
    Void,
    Bool,
    I32,
    F64,
    Str,      // native string (representation decided in)
    Dynamic,  // boundary-only boxed value; using it is an explicit opt-in
};
const char* typeName(Type t);

// What one `Op::PinGuard` tests, carried in the instruction's `immI32`.
//
// One enum per PIN KIND that has a read-side claim a write can contradict
// (src/types/pins.h). It lives here rather than in `types::PinKind` because
// the backend emits the test and the backend does not know about manifests:
// what reaches it is a SHAPE to check, not the declaration that asked for it.
enum class PinBarrier : uint8_t {
    // `<class>.<field>: number`, `function <fn>.<binding>: number`,
    // `param <owner>(<p>): number`, `return <owner>: number`, and the value
    // of a `numeric-elements` ELEMENT store. The read spends this claim on a
    // raw unbox, so a violation is a pointer's bits read as a double.
    Number,
    // `<class>.<field>: number-or-nullish`. The read stays boxed; what the
    // claim licenses is the branchless coercion, so a violation is a wrong
    // ToNumber rather than a wrong read.
    NumberOrNullish,
    // The FIELD half of `<class>.<field>: numeric-elements`: the slot holds a
    // plain JS Array. The element half is `Number`, checked at each element
    // store; see src/types/pins.h for what this pair does and does not reach.
    DenseArray,
};

// What one `Op::CensusRecord` observes, carried in the instruction's `immI32`
// and in the module's census site table (src/runtime/pin_census.h, stage C1).
//
// The low byte says which MANIFEST FORM the site's key names, because the
// forms admit different kinds and the kind is decided at exit from the tags. A
// census site exists only where lowering ran out of static answers: an env
// slot the fixpoint refused, a parameter no proof typed, a return the
// convention left Dynamic, a store to a field whose type is unknown. That is
// what makes the census complementary to the proofs rather than a duplicate of
// them (stage E4's HANDOFF (c)).
enum class CensusSite : uint8_t {
    EnvSlot = BRONZE_ABI_CENSUS_ENV_SLOT,
    Field = BRONZE_ABI_CENSUS_FIELD,
    Param = BRONZE_ABI_CENSUS_PARAM,
    Return = BRONZE_ABI_CENSUS_RETURN,
    // Not a form: a store to a field NAME through a receiver inference could
    // not type. It can never become an entry — it names no class — and what it
    // does is mark every entry for a field of that name `@observed`, because
    // B1's barrier cannot reach this store (src/types/pins.h).
    OpaqueFieldStore = BRONZE_ABI_CENSUS_OPAQUE,
};

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
    // is ordinary IL that would have existed anyway. The guarded-region pass
    // (src/lower/guard_region.h) emits it as the condition of a block's
    // terminator, and the block on the true edge is where the `unbox.f64 raw`
    // it licenses lives.
    IsNumber,   // a: bool = is.number b     (b: dynamic)
    Ret,        // ret [a]
    // `throw v`: stores v into the pending-exception cell and goes to this
    // block's handler. A terminator, because it is a way OUT of the block like
    // a jump — the edge it takes is just written on the block rather than on
    // the instruction.
    Throw,      // throw a
    // The pending value, taken and cleared. The first instruction of every
    // handler block, and the only way to read the cell: clearing it here is
    // what lets a `finally` run its body with nothing pending and then decide
    // whether to re-raise.
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
    DynamicImport,      // a = dynamic_import specifier
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
    // from it is the throw, and `canThrow` puts the exception check after it,
    // so the store this precedes is skipped on the violating path.
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
};
const char* opName(Op op);
bool isTerminator(Op op);

// The function selector `math.unary` carries in `immI32`. Membership is the
// soundness line: a function goes on this list only if IEEE 754 defines its
// result exactly, so backend intrinsic and runtime libm agree bit for bit.
enum class MathUnaryFn : uint32_t {
    Sqrt = 0,   // correctly rounded by IEEE 754 squareRoot
    Abs = 1,    // a sign-bit clear
    Floor = 2,  // roundToIntegralTowardNegative
    Ceil = 3,   // roundToIntegralTowardPositive
    Trunc = 4,  // roundToIntegralTowardZero
};

using ValueId = uint32_t;
inline constexpr ValueId kNoValue = UINT32_MAX;

using BlockId = uint32_t;
inline constexpr BlockId kNoBlock = UINT32_MAX;

struct BlockParam {
    ValueId id = kNoValue;
    Type type = Type::Void;
};

struct BlockTarget {
    BlockId block = kNoBlock;
    std::vector<ValueId> args;
};

struct Instruction {
    Op op;
    Type type = Type::Void;          // result type (Void: no result)
    ValueId result = kNoValue;
    std::vector<ValueId> operands;
    double immF64 = 0;               // ConstF64
    int32_t immI32 = 0;              // ConstI32 / CreateArray length / CreateFunction arity
    uint32_t calleeIndex = 0;        // Call/CreateFunction: index into Module::functions
    // The OPERAND's type, for the two ops whose behaviour turns on it and whose
    // own `type` is the result: Box (what is being boxed) and ToInt32 (whether
    // step 1's ToNumber is a machine conversion or a call that can throw).
    // Not printed for ToInt32 — `to.int32 %n` names one operation either way,
    // and the operand's own definition already says what type it is.
    Type boxType = Type::Void;
    uint32_t keyIndex = 0;           // PropGet/PropSet: key constant index
    uint32_t icIndex = 0;            // PropGet/PropSet: IC site index
    // PropGet: inference proved this site's receiver has ONE shape class, so
    // the backend may inline the cache check instead of calling the helper.
    // False is always sound — it is the plain call — and an unproven site keeps
    // it, so the inline form can never degenerate into a polymorphic guard
    // chain in generated code. It licenses the inline sequence; it does not
    // remove the guard. The shape word is the runtime's authority and a shape
    // class can name a layout the runtime never builds (`this.x =...` inside a
    // branch), so the guard is what makes the proof sound, not a redundancy on
    // top.
    bool icMonomorphic = false;
    // Unbox: the operand is PROVEN to be a Number, so the conversion is a
    // bitcast and nothing else — no tag test, no branch, no ToNumber helper, no
    // phi. bronze's Value is NaN-boxed with the doubles at the bottom of the
    // encoding, so a Number's bits ARE its double's bits: an unboxed f64 field
    // needs no second representation, only permission to stop checking.
    //
    // Granted by exactly one thing, `InferenceResult::provenFieldReads`: a read
    // of a field whose class installs it on every construction path, whose
    // receiver this compilation watched being made, and whose name the
    // whole-program write audit certified — or by a DOMINATING `is.number` on
    // this same SSA value, which is what the guarded-region pass emits: the
    // test is the condition of a block's terminator and the raw unbox sits in
    // the block on its true edge. On an SSA VALUE and never on a location — a
    // property re-read after a call is a different value and gets its own
    // guard. `false` is the checked form, which is ToNumber and correct for
    // anything, and is what every other Number claim in the lattice keeps.
    bool rawUnbox = false;
    // Unbox: the operand is PROVEN to be a Number, `null` or `undefined` and
    // nothing else, so ToNumber is decidable from the bits with no call. The
    // number range is the BOTTOM of the NaN box, so one unsigned compare
    // separates the arms, and the nullish arm is two constants — +0 for null
    // and NaN for undefined (7.1.4 table 14). Emitted branchless: the compares
    // and selects stay in the block, which is the point, because what this
    // exists for is the middle of a chain of fmuls.
    //
    // Mutually exclusive with `rawUnbox` and strictly weaker: a raw unbox of
    // `undefined` reads 0xFFF6000000000000 as a double. Granted by exactly one
    // thing, `InferenceResult::nullishNumberFieldReads` — a `--pins
    // number-or-nullish` declaration (types/pins.h) — and by no lattice type,
    // because there is no lattice element for "number or nullish" and there is
    // deliberately never going to be one.
    bool nullishUnbox = false;
    // PropGet/PropSet: the instance slot a PROVEN CLASS LAYOUT puts this key
    // at, or `kNoStaticSlot`. Strictly stronger than `icMonomorphic`, which is
    // only an identity claim: this says the receiver's class was modellable end
    // to end (every field it installs, in the runtime's own transition order,
    // through the whole `extends` chain) and that this key is one of the own
    // data properties in it — never a prototype method, never an accessor.
    //
    // What it licenses is a load at a COMPILE-TIME CONSTANT offset behind a
    // single shape compare, instead of decoding a cache entry's slot word. It
    // still does not remove the guard, and it cannot: the layout is derived
    // from source and the shape is built at run time. The two are reconciled at
    // `staticCellIndex` below.
    static constexpr uint32_t kNoStaticSlot = 0xFFFFFFFFu;
    uint32_t staticSlot = kNoStaticSlot;
    // The module-global cell this site compares the receiver's shape against.
    // One i64 per static site, zero-initialized, published by the runtime the
    // first time the site's slow path sees an object that really does carry
    // this key as an own data property at `staticSlot`. A layout that was
    // wrong therefore never publishes, and the site simply keeps taking its
    // slow path — the failure mode is a cost, never an answer.
    uint32_t staticCellIndex = 0;
    // PropGet/PropSet: the LAYOUT FAMILY this site guards on instead of one
    // shape's identity, as the preorder id of the receiver's class and the size
    // of its `extends` subtree. `kNoFamily` for a site that keeps the identity
    // form.
    //
    // This is what a `this` receiver inside a method of an EXTENDED class gets.
    // Its static class is not its runtime shape — three.js never constructs a
    // bare `Object3D`, so `this.matrixWorld` in `updateMatrixWorld` runs on a
    // Group, a Mesh and a Scene — and a cell that pins one of those misses on
    // the rest forever. The layout was never wrong: a proven subclass's fields
    // begin with its base's, at the same slots. So the guard changes shape
    // rather than the claim, from `shape == pinned` to `stamp - (base + lo) <=u
    // span`, where the stamp is a word the runtime wrote onto the SHAPE after
    // verifying that class's whole field list against it.
    //
    // `staticSlot` still says which slot; these say who may use it.
    static constexpr uint32_t kNoFamily = 0xFFFFFFFFu;
    uint32_t familyLo = kNoFamily;
    uint32_t familySpan = 0;
    // MethodCall: the module function this site's key resolves to, on the class
    // lowering believes the receiver has — a GUESS, and named as one, because
    // the receiver's static class is not its runtime shape and a subclass may
    // override the name.
    //
    // What makes the guess sound is that the backend does not spend it on the
    // receiver at all. The site's own inline cache has already established, for
    // a matching shape, WHICH function object the key resolves to and what its
    // code pointer is; the direct edge adds one compare of that code pointer
    // against this function's call wrapper. A wrong guess therefore never
    // matches and the site takes the cache's ordinary indirect dispatch — the
    // same failure mode a mispredicted class layout has, a cost and never an
    // answer.
    //
    // What it buys is everything the indirect call blocks: the argument vector
    // is never built, the wrapper's unpack never runs, and the callee is an
    // ordinary internal LLVM function at a call site LLVM may inline — which is
    // how a `setValue -> arraysEqual -> copyArray` chain becomes one region.
    static constexpr uint32_t kNoDirectTarget = 0xFFFFFFFFu;
    uint32_t directTarget = kNoDirectTarget;
    uint32_t envDepth = 0;           // EnvGet/EnvSet: parent hops
    uint32_t envIndex = 0;           // EnvGet/EnvSet: slot within that environment
    // A direct `call` to a CLOSURE, and the number of parent links from the
    // record in operand 0 to the one the closure captured.
    //
    // A direct call normally cannot reach a closure at all: `__env` arrives
    // through the dynamic calling convention, and a call site that named the
    // function without supplying one would enter it with garbage. This says the
    // site supplies it — not by loading the closure value, which would put the
    // whole guarded environment read back on the fast path, but by handing over
    // the CALLER's own record and a hop count the scope plan established
    // (lower_scope.cpp, `planStableFunctionSlots`). Operand 0 is that record,
    // and it occupies the callee's `__env` parameter.
    //
    // `kNoEnvHops` — the default — means the site supplies no environment, and
    // the verifier then holds the original rule: a direct call names a function
    // that needs none.
    static constexpr uint32_t kNoEnvHops = 0xFFFFFFFFu;
    uint32_t callEnvHops = kNoEnvHops;

    BlockTarget target;              // Jump target / Branch then-target
    BlockTarget elseTarget;          // Branch else-target
};

// Whether this instruction can leave an exception pending, and so needs the
// backend's cell test after it. Defined in print.cpp, beside `isTerminator`,
// because both are one-line facts about the op table.
bool canThrow(const Instruction& inst);

// Whether running this instruction can move a heap object: it allocates, or it
// reaches user code that might. STRICTLY WIDER than `canThrow` — an allocation
// that fails is fatal rather than catchable, so `create.object` throws nothing
// and collects anyway — and the two are not interchangeable at a use site.
//
// It exists for the backend's receiver proof (llvm_recv_proof.h), which holds a
// pointer DERIVED from a heap object across several instructions. A derived
// pointer is not a GC root: nothing forwards it, so it is valid exactly as far
// as the next collection, and this predicate is where that distance is
// measured.
//
// The default is TRUE. Only ops enumerated as neither allocating nor reaching
// user code answer false, so an op added later is a missed optimisation rather
// than a dangling pointer.
bool canCollect(const Instruction& inst);

// Which copy of a guarded numeric region a block belongs to
// (src/lower/guard_region.h). The pass duplicates a region into a FAST copy
// whose numbers are carried as `f64` and leaves the original blocks as the SLOW
// copy, entered only through one-way trampolines. The two are MUTUALLY
// EXCLUSIVE: control enters the fast copy at most once per region entry and,
// once it has left through a trampoline, never returns.
//
// It is a fact about the block and not about the function, so it lives here: it
// travels with the block through every copy, move and renumbering the pass and
// the pruner perform, which a parallel vector on `Function` would have to be
// kept in step with by hand at each of them.
//
// What reads it is `planFrame` (codegen-llvm/llvm_frame.h): two values that can
// never both be live may share one GC root slot, and the two copies of one
// region are exactly that. What makes that a proof rather than a hope is the
// verifier, which rejects a value defined in one copy and used in the other.
enum class CopyClass : uint8_t {
    Shared,  // outside every region — including a region's preheader and exits
    Fast,    // the fast copy proper, its guard chains and its trampolines
    Slow,    // an original region block, or the tail of one split for a guard
};

// `Block::copyRegion` when the block belongs to no region's copy.
inline constexpr uint32_t kNoCopyRegion = UINT32_MAX;

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
    // Which copy this block is, and of WHICH region: one function can hold
    // several disjoint fast/slow pairs, and only the two copies of the SAME
    // region are known to be mutually exclusive. `Shared` always carries
    // `kNoCopyRegion`; the two together are the block's copy identity.
    CopyClass copyClass = CopyClass::Shared;
    uint32_t copyRegion = kNoCopyRegion;
    // Where control goes if an exception becomes pending inside this block: the
    // innermost enclosing handler, or `kNoBlock` for "leave the function". It
    // sits on the block rather than on each call so that lowering emits no test
    // at all — the backend derives them from `canThrow`, and the block is never
    // split in the IL, so no join here grows a parameter.
    //
    // A handler block therefore takes NO parameters: it is entered from an
    // arbitrary point in the protected region, and nothing here knows what a
    // binding held there, which is what an environment record makes possible.
    BlockId handler = kNoBlock;
};

struct Param {
    std::string name;
    Type type;
    // This slot is `F64` because a `--pins` SIGNATURE entry said so, and no
    // proof agrees (src/types/pins.h, stage B1). It is the licence for the
    // barrier the boxed wrapper and the enumerated call sites emit, and it is
    // deliberately NOT "the type is F64": a parameter typed by
    // `applyProvenSignature` or by the closure parameter proof has a proof
    // behind it, and re-checking a proof would tax the programs that need no
    // barrier at all. The pin index names the manifest line for the message.
    bool pinned = false;
    // The key-constant index of the manifest line this pin came from, valid
    // only when `pinned`.
    uint32_t pinKeyIndex = 0;
};

struct Function {
    std::string name;
    // Synthetic leading parameters come first, in this order: [__env if
    // needsEnv] [__this if needsThis] [__arguments if needsArguments] source
    // parameters... __env can only come from the closure, so a needsEnv
    // function is never a direct-call target; __this is supplied by every
    // caller, including direct ones, so needsThis carries no such restriction.
    std::vector<Param> params;
    Type returnType = Type::Void;
    // `returnType` is `F64` because a `--pins` `return <owner>: number` entry
    // said so, with no proof agreeing — the return half of `Param::pinned`,
    // and the licence for the barrier `lowerReturnStmt` puts on a returned
    // value it cannot type. `pinKeyIndex` names the manifest line.
    bool returnPinned = false;
    uint32_t returnPinKeyIndex = 0;
    bool isExported = false;
    // The module's entry point. Everything else about a function is the same,
    // and one thing is not: it has no caller to propagate an exception to, so
    // its unwind path reports the value and exits instead of returning. A flag
    // rather than a name comparison in the backend, because "which function is
    // `main`" is a fact lowering knows and codegen should not have to spell.
    bool isEntryPoint = false;
    bool needsEnv = false;
    bool needsThis = false;
    // `arguments`: every argument the caller really passed, as one array. Like
    // the rest array it can only be built where the true argument count is
    // visible, which is the call WRAPPER — so a function that needs one is
    // never a direct-call target, the same restriction `needsEnv` carries and
    // for a related reason.
    bool needsArguments = false;
    bool isStrict = false;
    bool isGenerator = false;
    // The BODY of a generator or async function, lowered as a resume machine:
    // `<name>.resume(__env, __mode, __sent)`, whose entry block dispatches on a
    // resume index held in the frame (src/lower/lower_generator.cpp).
    //
    // It is a fact lowering knows and nothing downstream can read off the
    // shape: the dispatch is an ordinary compare-and-branch chain, its targets
    // are ordinary blocks, and a function that happens to start with a switch
    // over a number is indistinguishable from it. The guarded-region pass is
    // what asks — a whole-function duplication of a resume machine copies a
    // state machine whose live values cross suspensions in the FRAME rather
    // than in SSA, so the promotion has nothing to carry and the copy is pure
    // growth.
    bool isResumeBody = false;
    // The BRONZE_ABI_FN_FLAG_* byte the created function object carries: what
    // its syntax decided about [[Construct]] and about the `prototype`
    // property (src/abi/bronze_abi.h says why neither is derivable here).
    // `isGenerator` above is a different question — it selects the frame plus
    // resume machine this body is lowered INTO — and the two are set from the
    // same AST node without either being read off the other.
    uint32_t fnFlags = BRONZE_ABI_FN_FLAGS_ORDINARY;
    // `...rest`: the LAST source parameter, and the one no caller supplies a
    // value for. It arrives as an array built from whatever arguments were left
    // over — by the call wrapper on the uniform path, by the call site on a
    // direct one.
    bool hasRestParam = false;
    // The parameters before the first one with a default. NOT a minimum a call
    // has to meet — the language has no arity error, and a call short of this
    // is filled with `undefined` at the call site exactly as one short of the
    // fixed parameter count is, with the callee's prologue deciding what that
    // means.
    //
    // It is also, exactly, ECMA-262 15.1.5 ExpectedArgumentCount — so it is
    // what 10.2.10 SetFunctionLength makes the function's `length` property,
    // and the backend passes it through as such. Two facts, one count, and
    // deliberately not two fields: a program that could see them disagree would
    // be seeing a bug.
    uint32_t requiredArgs = 0;
    // 10.2.9 SetFunctionName's answer for this function, as an index into
    // `Module::keyConstants`. It is NOT `name` above: that is the IL's own
    // identifier, synthesized as `__anon_fn_N` for a function the source did
    // not name, while this is the string the language says `f.name` is — "" for
    // a genuinely anonymous function, the binding's name under NamedEvaluation
    // (8.6.2), and "get x" / "set x" for an accessor.
    //
    // `BRONZE_ABI_FN_NAME_NONE` means the name was never recorded, which a
    // program can only reach for a function whose name is computed at runtime
    // (`class C { [k]() {} }`); reading `.name` off one is then a diagnosed
    // refusal rather than a wrong answer.
    uint32_t nameKeyIndex = 0xFFFFFFFFu;
    // The bytes of source this function was written from, as a half-open range
    // into `Module::sourceTexts[sourceFile]`. It is what 20.2.3.5
    // Function.prototype.toString returns, which the spec makes the SOURCE
    // TEXT verbatim and not a reconstruction — so nothing downstream may
    // re-render it, and the range has to be exact: `sourceEnd` is the end of
    // the function's last token, never the start of the next one.
    //
    // For a class it is the whole `class C { ... }`, because the constructor
    // IS the class value and `C.toString()` is the class's text (15.7.14 makes
    // the class's source text the constructor's [[SourceText]]).
    //
    // `sourceEnd == 0` means no text was recorded — a function bronze
    // synthesized, or a build that passed `--no-fn-source`.
    uint16_t sourceFile = 0;
    uint32_t sourceBegin = 0;
    uint32_t sourceEnd = 0;

    // Index of the first source-level parameter.
    size_t firstSourceParam() const {
        return static_cast<size_t>(needsEnv) + static_cast<size_t>(needsThis) +
               static_cast<size_t>(needsArguments);
    }
    // How many arguments a CALLER passes: every source parameter but the
    // rest one, which is not a value the convention carries.
    size_t callerParamCount() const {
        return params.size() - firstSourceParam() - static_cast<size_t>(hasRestParam);
    }
    // The arity a short call is PADDED to with `undefined`. Zero means "do not
    // pad", which is what a function owning an `arguments` object needs: the
    // object is built from the argument count the wrapper sees, and padding
    // would make `f(1)` indistinguishable from `f(1, undefined)` where the
    // language says `arguments.length` is 1 and 2. Its wrapper reads argv
    // through `bronze_arg_at` instead.
    uint32_t adaptArity() const {
        return needsArguments ? 0u : static_cast<uint32_t>(callerParamCount());
    }
    std::vector<Block> blocks;
    uint32_t valueCount = 0;  // number of ValueIds in use (params first)
};

struct Module {
    std::string name;
    std::vector<std::string> keyConstants;
    // How many inline-cache sites lowering handed out across the whole module.
    // The backend emits exactly this many entries as a global array in the
    // object file, so it is the size of a real allocation, not a hint: the
    // verifier checks every icIndex against it.
    uint32_t icSiteCount = 0;
    // How many TAGGED TEMPLATE sites lowering handed out. Sized the same way
    // `icSiteCount` is and for the same reason: the backend emits exactly this
    // many Value cells as a global array, and each site's cell holds the one
    // template object 13.2.8.4 GetTemplateObject requires that site to hand its
    // tag function on every call.
    uint32_t templateSiteCount = 0;
    // How many STATIC-SLOT sites lowering handed out. Sized exactly like
    // `icSiteCount`: the backend emits this many i64 cells as a global array,
    // and the verifier checks every `staticCellIndex` against it. Zero when the
    // seam is off or nothing proved, and then no array is emitted at all.
    uint32_t staticSiteCount = 0;
    // The proven class layouts, in the PREORDER the family ids number: what the
    // module hands `bronze_register_class_family` at init so that the runtime
    // can recognise a shape as an instance of one. Empty when nothing proved,
    // or when the family seam is off.
    struct ClassFamilyField {
        uint32_t keyIndex = 0;   // into `keyConstants`
        bool writable = true;    // as the construction sequence installs it
    };
    struct ClassFamilyEntry {
        std::string name;  // the class's binding, for the IL dump only
        std::vector<ClassFamilyField> fields;
    };
    std::vector<ClassFamilyEntry> classFamilies;
    // THE SLOT-REPRESENTATION ELIGIBILITY LIST (stage R1,
    // src/runtime/slot_repr.h): indices into `keyConstants` naming the property
    // names a `--pins` manifest declared `number` on some class this
    // compilation proved a layout for. The runtime is told them at module init
    // and uses them for one decision — whether a shape transition that FIRST
    // installs such a name, with a Number in hand, may store the slot as a raw
    // double instead of a boxed Value.
    //
    // NAMES and not (class, slot) pairs: the runtime meets a transition long
    // before it can recognise a class, and the name is the only fact available
    // at the moment the storage decision has to be made. Over-application is
    // sound — the runtime generalizes a slot whose promise a store breaks — so
    // a name shared with an unpinned class costs a shape split at worst.
    // Empty without a manifest, and empty under BRONZE_NO_SLOT_REPR=1.
    std::vector<uint32_t> slotReprFields;
    // THE PIN CENSUS SITE TABLE (`--census`, src/runtime/pin_census.h). One
    // entry per site lowering created, handed to the runtime at module init so
    // that a site the run never reaches is still known — "never observed" and
    // "not a site" are different answers, and a STATIC refusal has to
    // disqualify its entry on a run that never touches it.
    //
    // Built here rather than scanned back out of the instructions, because
    // some entries have no instruction at all: an owner spelling that would
    // govern two different IL functions is refused by a table row and nothing
    // else.
    struct CensusSiteEntry {
        uint32_t keyIndex = 0;  // into `keyConstants`
        uint32_t info = 0;      // BRONZE_ABI_CENSUS_* kind | flags
    };
    std::vector<CensusSiteEntry> censusSites;
    // Where the census run writes its manifest. Empty unless `--census`.
    std::string censusOutPath;
    // A deque, not a vector: lowering a function body can append nested
    // closures, and the body being lowered is itself an element. Only a
    // reference-stable container lets a recursive call read its own
    // (still-being-inferred) signature without dangling on a reallocation.
    std::deque<Function> functions;
    // Every source file of the program, indexed by `Function::sourceFile`, so
    // that a function's `sourceBegin`/`sourceEnd` name real bytes. ONE COPY of
    // each file rather than a string per function: three.js's 28 files are
    // 1.6 MB together, and the ~3000 functions in them overlap almost
    // completely, so per-function strings would be that figure many times over.
    //
    // Empty when the build asked for no function source, which is the whole of
    // what `--no-fn-source` does — the ranges above stay, and address nothing.
    std::vector<std::string> sourceTexts;
};

// The PINNED element kind: a plain dense JS array whose reads and writes are
// ASSUMED in-bounds, hole-free and numeric — no guard of any kind is emitted.
// Granted by a `--pins ... numeric-elements` declaration (types/pins.h), or by
// the blanket `BRONZE_UNSOUND_PINS` measurement mode. Nothing PROVES the
// assumption; enforcement is meant to move to the write paths. Deliberately
// outside the types::TypedArrayElem range so nothing sound can collide with it.
inline constexpr int32_t kElemKindPlainArrayF64 = 100;

}  // namespace bronze::il
