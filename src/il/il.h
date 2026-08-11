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
    Mul,
    Div,
    Mod,
    CmpLt,      // a: bool = cmp.lt b, c
    CmpGt,
    CmpEq,
    CmpNe,
    StrictEq,   // a: bool = strict.eq b, c   (JS ===, both operands dynamic)
    IsNullish,  // a: bool = is.nullish b
    Ret,        // ret [a]
    Jump,       // jump bN(args...)
    Branch,     // br %cond, bThen(args...), bElse(args...)
    Call,       // a = call <funcRef>(args...)
    Box,        // a = box.<type> b
    Unbox,      // a = unbox.<type> b
    PropGet,    // a = prop.get b, <key_const_index>, <ic_site_index>
    PropSet,    // prop.set b, <key_const_index>, c, <ic_site_index>
    ElemGet,    // a = elem.get obj, idx        (both dynamic; computed index)
    ElemSet,    // elem.set obj, idx, val       (all dynamic)
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
    Construct,  // a = new callee, args...              (docs/0008)
    CreateObject, // a = create.object
    ObjectKeys, // a = object.keys b                  (docs/0009)
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
    CreateArray,  // a = create.array <length>
    CreateFunction,// a = create.func <funcIndex>, env
    FunctionRef,   // a = func.ref <funcIndex>          (docs/0008)
    EnvCreate,  // a = env.create parent, <slots>     (docs/0007)
    EnvGet,     // a = env.get env, <depth>, <index>
    EnvSet,     // env.set env, <depth>, <index>, v
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

struct Block {
    BlockId id = 0;
    std::vector<BlockParam> params;
    std::vector<Instruction> instructions;
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
    bool needsEnv = false;
    bool needsThis = false;

    // Index of the first source-level parameter.
    size_t firstSourceParam() const {
        return static_cast<size_t>(needsEnv) + static_cast<size_t>(needsThis);
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
