#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace bronze::il {

// bronze IL: a typed, SSA, mid-level IR. Static layouts are used wherever
// analysis can PROVE them (inference-first; TS annotations are untrusted
// hints); Dynamic is the explicit fallback type for code the analysis
// cannot type — wild JS must always compile. The inverse of broc's design,
// where dynamic was the substrate rather than the fallback.
//
// Deliberately tiny today: enough structure to carry lower→codegen work and
// to pin the canonical text form. Every addition must keep print(parse(x))
// byte-stable once the text parser lands.

enum class Type : uint8_t {
    Void,
    Bool,
    I32,
    F64,
    Str,      // native string (representation decided in docs/0004)
    Dynamic,  // boundary-only boxed value; using it is an explicit opt-in
};
const char* typeName(Type t);

enum class Op : uint8_t {
    ConstF64,   // a = const.f64 <imm>
    ConstI32,   // a = const.i32 <imm>
    ConstBool,  // a = const.bool <imm>
    ConstUndefined, // a: dynamic = const.undefined
    ConstNull,      // a: dynamic = const.null
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
    // The bitwise family (docs/0015). Operands are i32 — lowering puts a
    // `to.int32` in front of every one of them — and the RESULT is the JS
    // number that int32 denotes, so these read `%n: f64 = and %i32, %i32`.
    // Keeping the result an i32 would leak a type inference has no element
    // for into block joins and calling conventions; the int32 is an
    // intermediate of the operator and never escapes it.
    ToInt32,    // a: i32 = to.int32 b        (b: f64, bool or dynamic)
    BitAnd,     // a: f64 = and b, c
    BitOr,
    BitXor,
    Shl,        // a: f64 = shl b, c          (count masked to 5 bits)
    Shr,        // arithmetic: the sign bit is replicated
    UShr,       // logical, and the ONE bitwise op whose result is ToUint32
    CmpLt,      // a: bool = cmp.lt b, c
    CmpGt,
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
    TypeOf,     // a: dynamic = typeof b      (one of six strings)
    InstanceOf, // a: bool = instanceof b, c
    In,         // a: bool = in b, c          (b: key, c: object)
    IsNullish,  // a: bool = is.nullish b
    Ret,        // ret [a]
    // `throw v`: stores v into the pending-exception cell and goes to this
    // block's handler (docs/0020 decision 3). A terminator, because it is a
    // way OUT of the block like a jump — the edge it takes is just written on
    // the block rather than on the instruction.
    Throw,      // throw a
    // The pending value, taken and cleared. The first instruction of every
    // handler block, and the only way to read the cell: clearing it here is
    // what lets a `finally` run its body with nothing pending and then decide
    // whether to re-raise (docs/0020 decision 5).
    ExcTake,    // a: dynamic = exc.take
    Jump,       // jump bN(args...)
    Branch,     // br %cond, bThen(args...), bElse(args...)
    Call,       // a = call <funcRef>(args...)
    Box,        // a = box.<type> b
    Unbox,      // a = unbox.<type> b
    PropGet,    // a = prop.get b, <key_const_index>, <ic_site_index>
    // `super.k`: a read of the PARENT prototype's property with `this` as
    // the receiver. Identical to prop.get for a method — the value is the
    // same function either way — and not identical at all for an accessor,
    // whose getter would otherwise run with the prototype as its receiver
    // (docs/0019 decision 3). No inline cache: the receiver and the holder
    // are different objects, and an entry describes one shape.
    SuperGet,   // a = super.get proto, <key_const_index>, thisArg
    PropSet,    // prop.set b, <key_const_index>, c, <ic_site_index>
    ElemGet,    // a = elem.get obj, idx        (both dynamic; computed index)
    ElemSet,    // elem.set obj, idx, val       (all dynamic)
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
    Construct,  // a = new callee, args...              (docs/0008)
    CreateObject, // a = create.object
    ObjectKeys, // a = object.keys b                  (docs/0009)
    // The keys a `for-in` will visit, as one array built before the first
    // iteration: own AND inherited enumerable string keys, each once
    // (docs/0018 decision 1). Snapshotting is what lets the loop itself be
    // for-of's index walk over the result, and it is a legal answer to the
    // spec's open question about mutation during enumeration.
    ForInKeys,  // a = forin.keys b
    // A class method: a property write with `enumerable: false`, which an
    // ordinary `prop.set` cannot express and which is what keeps a method out
    // of `Object.keys` and `for-in` (docs/0018 decision 2). No IC index — a
    // class body runs once.
    MethodDef,  // method.def obj, <key_const_index>, v
    // An accessor property: one property with two halves, either of which
    // may be `undefined` here because the source wrote only one of them
    // (docs/0019 decision 4). `immI32` is the enumerable attribute — 1 for
    // an object literal's accessor, 0 for a class's, the same split methods
    // already have. No IC index, for the reason `method.def` has none.
    AccessorDef,  // accessor.def obj, <key_const_index>, getter, setter, <enumerable>
    // `delete o.k` and `delete o[i]`. A reference operation, not a read:
    // the operand's property is never loaded, and the result is the boolean
    // ECMA-262 13.5.1 defines rather than the property's value.
    PropDelete,  // a: bool = prop.delete obj, <key_const_index>
    ElemDelete,  // a: bool = elem.delete obj, idx
    GlobalGet,  // a = global.get <key_const_index>   (docs/0011)
    // `class D extends B`: links D.prototype's proto to B.prototype and
    // D's static properties to B's. One op because both links have to
    // be made together, before any method is stored (docs/0012 dec. 5).
    ClassExtend, // class.extend derived, base
    // for-of, as an index walk (docs/0012 decision 2). Three ops rather
    // than one because a string iterates by CODE POINT: the step is not
    // always one, so the advance cannot be an `add 1` in the IL.
    IterLength,  // a: f64 = iter.length b
    IterAt,      // a = iter.at b, %index
    IterAdvance, // a: f64 = iter.advance b, %index
    // Everything from %index on, as a fresh array — a rest element's value
    // (docs/0017 decision 2). The same walk IterAt/IterAdvance do, run to the
    // end in the runtime, because its length is not known here.
    IterRest,    // a = iter.rest b, %index
    // The source of a destructuring, checked once before any element is read
    // (docs/0017 decision 4). `immI32` names which pattern asked, so the
    // diagnostic can say `array destructuring` rather than `for-of`, and the
    // check is what lets every element read below it assume a walkable value.
    PatternCheck,  // a = pattern.check b, <kind>
    // The three container-building ops a spread needs: one element, then all
    // of an iterable's, then all of an object's own enumerable properties
    // (docs/0017 decision 3). Appending rather than indexing is the point —
    // a literal with a spread in it has no length until it is built.
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
    ConstructSpread,    // a = new.spread callee, args
    CreateArray,  // a = create.array <length>
    CreateFunction,// a = create.func <funcIndex>, env
    FunctionRef,   // a = func.ref <funcIndex>          (docs/0008)
    EnvCreate,  // a = env.create parent, <slots>     (docs/0007)
    EnvGet,     // a = env.get env, <depth>, <index>
    EnvSet,     // env.set env, <depth>, <index>, v
    // The MODULE scope's environment record, which is a singleton: the top
    // level runs exactly once, so there is exactly one activation of that
    // scope and no reason to thread it through every calling convention
    // (docs/0016 decision 1). `main` publishes it; a module function that
    // needs a module-level binding loads it at entry and chains its own
    // record to it.
    ModuleEnvSet,  // module.env.set %env
    ModuleEnvGet,  // a: dynamic = module.env.get
    CreateArrayBuffer,  // a = create.arraybuffer len      (len dynamic)
    CreateFloat32Array, // a = create.f32array arg         (length or buffer, dynamic)
    Print,      // print a
};
const char* opName(Op op);
bool isTerminator(Op op);

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
    Type boxType = Type::Void;       // Box: input type being boxed (F64, I32, Bool, Str)
    uint32_t keyIndex = 0;           // PropGet/PropSet: key constant index
    uint32_t icIndex = 0;            // PropGet/PropSet: IC site index
    // PropGet: inference proved this site's receiver has ONE shape class
    // (docs/0010 decision 4), so the backend may inline the cache check
    // instead of calling the helper. False is always sound — it is the
    // plain call — and an unproven site keeps it, so the inline form can
    // never degenerate into a polymorphic guard chain in generated code.
    // It licenses the inline sequence; it does not remove the guard. The
    // shape word is the runtime's authority and a shape class can name a
    // layout the runtime never builds (`this.x = ...` inside a branch), so
    // the guard is what makes the proof sound, not a redundancy on top.
    bool icMonomorphic = false;
    uint32_t envDepth = 0;           // EnvGet/EnvSet: parent hops
    uint32_t envIndex = 0;           // EnvGet/EnvSet: slot within that environment

    BlockTarget target;              // Jump target / Branch then-target
    BlockTarget elseTarget;          // Branch else-target
};

// Whether this instruction can leave an exception pending, and so needs the
// backend's cell test after it (docs/0020 decision 1). Defined in print.cpp,
// beside `isTerminator`, because both are one-line facts about the op table.
bool canThrow(const Instruction& inst);

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
    // Where control goes if an exception becomes pending inside this block:
    // the innermost enclosing handler, or `kNoBlock` for "leave the function"
    // (docs/0020 decision 3). It sits on the block rather than on each call
    // so that lowering emits no test at all — the backend derives them from
    // `canThrow`, and the block is never split in the IL, so no join here
    // grows a parameter.
    //
    // A handler block therefore takes NO parameters: it is entered from an
    // arbitrary point in the protected region, and nothing here knows what a
    // binding held there. Decision 4 is what makes that possible.
    BlockId handler = kNoBlock;
};

struct Param {
    std::string name;
    Type type;
};

struct Function {
    std::string name;
    // Synthetic leading parameters come first, in this order:
    //   [__env if needsEnv] [__this if needsThis] source parameters...
    // __env can only come from the closure, so a needsEnv function is
    // never a direct-call target (docs/0007); __this is supplied by every
    // caller, including direct ones, so needsThis carries no such
    // restriction (docs/0008 decision 3).
    std::vector<Param> params;
    Type returnType = Type::Void;
    bool isExported = false;
    // The module's entry point. Everything else about a function is the
    // same, and one thing is not: it has no caller to propagate an exception
    // to, so its unwind path reports the value and exits instead of returning
    // (docs/0020 decision 2). A flag rather than a name comparison in the
    // backend, because "which function is `main`" is a fact lowering knows
    // and codegen should not have to spell.
    bool isEntryPoint = false;
    bool needsEnv = false;
    bool needsThis = false;
    // `...rest`: the LAST source parameter, and the one no caller supplies a
    // value for. It arrives as an array built from whatever arguments were
    // left over — by the call wrapper on the uniform path, by the call site
    // on a direct one (docs/0017 decision 2).
    bool hasRestParam = false;
    // How many source arguments a call must supply: the parameters before the
    // first one with a default. Fewer is a diagnosed arity error; anything
    // between this and the fixed parameter count is filled with `undefined`
    // at the call site, and the callee's prologue decides what that means.
    uint32_t requiredArgs = 0;

    // Index of the first source-level parameter.
    size_t firstSourceParam() const {
        return static_cast<size_t>(needsEnv) + static_cast<size_t>(needsThis);
    }
    // How many arguments a CALLER passes: every source parameter but the
    // rest one, which is not a value the convention carries.
    size_t callerParamCount() const {
        return params.size() - firstSourceParam() - static_cast<size_t>(hasRestParam);
    }
    std::vector<Block> blocks;
    uint32_t valueCount = 0;  // number of ValueIds in use (params first)
};

struct Module {
    std::string name;
    std::vector<std::string> keyConstants;
    // How many inline-cache sites lowering handed out across the whole
    // module. The backend emits exactly this many entries as a global array
    // in the object file (docs/0010 decision 7), so it is the size of a
    // real allocation, not a hint: the verifier checks every icIndex
    // against it.
    uint32_t icSiteCount = 0;
    // A deque, not a vector: lowering a function body can append nested
    // closures, and the body being lowered is itself an element. Only a
    // reference-stable container lets a recursive call read its own
    // (still-being-inferred) signature without dangling on a reallocation.
    std::deque<Function> functions;
};

}  // namespace bronze::il
