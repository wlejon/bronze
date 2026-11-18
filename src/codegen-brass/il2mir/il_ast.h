#pragma once

#include <string>
#include <vector>
#include <cstdint>

// il2mir: bronze's IL → brass MIR translator. bronze::il::Module is
// flattened into this AST (il_to_brass_ast.cpp), then lowered to a
// brass::Module against bronze's runtime ABI (src/abi/bronze_abi.h). The
// translator is written against brass's MIR builder throughout, so the
// namespace sees brass's names unqualified; it sits outside `bronze` so
// bronze's own Module/Value/Type never shadow them.
namespace brass {}
namespace il2mir {
using namespace brass;
}

namespace il2mir {

enum class BronzeType {
    Void,
    Bool,
    I32,
    F64,
    Str,
    Dynamic,
    // il::Type::Ptr: a raw machine word no collector reads (a shape's
    // address), held as an i64.
    Ptr,
    Unknown
};

enum class BronzeOp {
    ConstF64,
    ConstI32,
    ConstBool,
    ConstUndefined,
    ConstNull,
    ConstBigInt,
    Add,
    Sub,
    Neg,
    Mul,
    Div,
    Mod,
    Pow,
    BitAnd,
    BitOr,
    BitXor,
    Shl,
    Shr,
    UShr,
    BitNot,
    ToInt32,
    ToNumeric,
    NumericStep,
    CmpLt,
    CmpGt,
    CmpLe,
    CmpGe,
    CmpEq,
    CmpNe,
    StrictEq,
    LooseEq,
    RelLt,
    RelGt,
    RelLe,
    RelGe,
    NumTruthy,
    TypeOf,
    ToStr,
    Box,
    Unbox,
    Call,
    CallDynamic,
    NameResolve,
    EnvCreate,
    EnvGet,
    EnvSet,
    EnvGetTdz,
    EnvInitTdz,
    CreateFunc,
    CreateArray,
    CreateObject,
    PropGet,
    PropSet,
    ElemGet,
    ElemSet,
    MethodDef,
    Print,
    PrintErr,
    Ret,
    Jump,
    Branch,
    Throw,
    ExcTake,
    CoroStart,
    CoroSuspend,
    CoroMode,
    IterOpen,
    IterStep,
    ModuleEnvSet,
    ModuleEnvGet,
    ConcatBegin,
    ConcatAppend,
    ConcatEnd,
    IsNumber,
    IsDenseArray,
    IsNullish,
    GlobalGet,
    MethodCall,
    Construct,
    FuncRef,
    ClassExtend,
    SuperCall,
    SuperGet,
    InstanceOf,
    In,
    PinGuard,
    CensusRecord,
    SuperSet,
    DynamicImport,
    ModuleNamespace,
    ForInKeys,
    ForInGuard,
    ForInSame,
    ForInLive,
    MethodDefComputed,
    AccessorDef,
    AccessorDefComputed,
    DefineOwnAttr,
    GetNewTarget,
    ImportMeta,
    SuperCallSpread,
    TemplateCached,
    TemplateObject,
    PropDelete,
    ElemDelete,
    ImmutableAssign,
    PrivateNew,
    PrivateHas,
    PrivateGet,
    PrivateAdd,
    PrivateSet,
    PrivateMisuse,
    AsyncIterOpen,
    AsyncIterNext,
    AsyncIterClose,
    IterValue,
    IterClose,
    IterRest,
    IterDelegate,
    PatternCheck,
    ArrayAppend,
    ArraySpread,
    ObjectSpread,
    ObjectRest,
    DynamicCallSpread,
    MethodCallSpread,
    ApplyArgs,
    ConstructSpread,
    PrintSpread,
    PrintSpreadErr,
    KeepAlive,
    IsTypedArray,
    IsPlainArray,
    IsCollection,   // is.collection (il_op.h): imm_i64 the class
    CollectionOp,   // collection.op (il_op.h): imm_i64 the op
    // `prop.set` / `elem.set` in kWriteDefine mode (il.h): CreateDataPropertyOrThrow.
    PropDefine,
    ElemDefine,
    ObjectLitProto,
    // Speculation (il_op.h, "leaving a speculated function"), lowered in
    // il_lowering_resume.cpp. ResumePoint's, Leave's and ResumeCall's id is
    // `imm_i64`; Leave's and ResumeCall's target G is `callee_name` and its
    // AST index `index`.
    ResumePoint,
    Leave,
    ResumeCall,
    // The speculated function's low-level forms (il_op.h), lowered in
    // il_lowering_spec.cpp. Their immediates: LoadShape none, ShapeCell the
    // cell in `imm_i64`, SlotLoad / SlotStore the slot in `imm_i64`, FnCodeIs
    // the function in `callee_name` and its AST index in `index`.
    LoadShape,
    ShapeCell,
    PtrEq,
    ShapeIn,
    // The slot and write bit packed in `imm_i64` as il_op.h's `immI32`, the
    // key's module index in `index`.
    ShapeHasOwn,
    // The key's module index in `index`; operands the object and its shape.
    ShapeLacks,
    // The operand a shape; no immediate.
    ShapeProto,
    IsObject,
    SlotLoad,
    SlotStore,
    IsI32,
    UnboxI32,
    UnboxF64,
    I32ToF64,
    AddWouldOvf,
    SubWouldOvf,
    MulWouldOvf,
    FnCodeIs,
    FnEnv,
    ArrayLength,
    ElemLoad,
    ElemStore,
    InBounds,
    IsHole,
    IsUninit,
    // il_op.h, "kinds, typed arrays and intrinsics": TypedLoad / TypedStore
    // carry the element kind, FnNativeIs / MathFn the il::Intrinsic, in
    // `imm_i64`.
    IsArray,
    TypedLength,
    TypedLoad,
    TypedStore,
    FnNativeIs,
    MathFn,
    // il_op.h's literal and construction forms: ObjectLiteral the literal in
    // `imm_i64`, ProtoCell / ProtoPublish / MemberCell / MemberPublish the
    // cell in `imm_i64` (MemberPublish its cell's function in `index` and
    // `callee_name`).
    ObjectLiteral,
    ArrayLiteral,
    FnConstructs,
    FnInstanceShape,
    CreateThis,
    ShapeStore,
    ProtoCell,
    ProtoPublish,
    MemberCell,
    MemberPublish,
    ConstructResult,
    ConcatN,
    TypeOfIs,
    ArrayPush,
    Unknown
};

// The write mode a PropDefine hands the shared set lowering in place of a
// strict flag: il.h kWriteDefine (il_to_brass_ast.cpp asserts they agree).
// Every helper choice keyed off the mode routes it to bronze_prop_define.
// `to.string`'s imm bit il::kToStrKeepNumber: a Number passes unconverted to
// the concat.n that uses it.
inline constexpr int64_t kToStrKeepNumber = 1;
inline constexpr uint32_t kDefineWriteMode = 2;
// il.h kWriteLiteralElement: an array literal's element store, an
// assignment's `PropSet` whose miss defines (bronze_prop_define).
inline constexpr uint32_t kLiteralElementWriteMode = 3;

struct BronzeBlockTarget {
    uint32_t block_id = 0;
    std::vector<uint32_t> args;
};

struct BronzeInstruction {
    uint32_t result_id = UINT32_MAX;
    BronzeType result_type = BronzeType::Void;
    BronzeOp op = BronzeOp::Unknown;
    std::vector<uint32_t> operands;

    double imm_f64 = 0.0;
    int64_t imm_i64 = 0;
    bool imm_bool = false;
    BronzeType box_type = BronzeType::Unknown;
    std::string callee_name;
    std::string string_literal;

    uint32_t depth = 0;
    uint32_t index = 0;
    uint32_t param_count = 0;

    static constexpr uint32_t kNoIcIndex = UINT32_MAX;
    uint32_t ic_index = kNoIcIndex;
    // il.h `slowSite`: the instruction's slow-site counter, or kNoSlowSite.
    static constexpr uint32_t kNoSlowSite = UINT32_MAX;
    uint32_t slow_site = kNoSlowSite;
    // The inlined call the instruction's code came from: 1-based into its
    // function's `inline_scopes`, 0 for the function's own code.
    uint32_t inline_scope = 0;

    BronzeBlockTarget target;
    BronzeBlockTarget else_target;

    uint32_t line = 0;
    uint32_t column = 0;
    // The index into BronzeModuleAST::source_files that `line` is in, or
    // UINT32_MAX for the module's own name (the instruction has no span).
    uint32_t file = UINT32_MAX;
    uint32_t env_hops = UINT32_MAX;
};

struct BronzeBlock {
    uint32_t id = 0;
    std::vector<std::pair<uint32_t, BronzeType>> params;
    uint32_t handler_id = UINT32_MAX;
    // il::Block::cold: laid out after the function's main line.
    bool cold = false;
    std::vector<BronzeInstruction> instructions;
};

struct BronzeFunction {
    std::string name;
    std::vector<std::pair<uint32_t, BronzeType>> params;
    BronzeType return_type = BronzeType::Void;
    bool is_exported = false;
    // A native import (il.h Function::isExternal): no body anywhere, raw
    // bits in and out.
    bool is_import = false;
    // A coroutine body's BRONZE_ABI_CORO_* kind, or -1: the function runs on
    // a brass coroutine frame (its leading parameter) and is reached only
    // through `coro_create`.
    int32_t coro_kind = -1;
    bool is_coroutine_body() const { return coro_kind >= 0; }
    // The program's top level (`main` and its segments): run once.
    bool is_toplevel = false;
    // il.h Function::entrySlowSite: the entry count's index (UINT32_MAX: none).
    uint32_t entry_slow_site = UINT32_MAX;
    // Compiled as brass's cold functions are (Function::is_cold): the
    // harvest the build spent saw it run seldom or never (il.h
    // Function::heat; il_to_brass_ast.cpp).
    bool cold = false;
    // A cold function's ENTRY COUNTER in the image's cold-entry table
    // (TranslatorOptions::cold_entry_count; UINT32_MAX: none): what says,
    // after a run of a module built on a harvest, that the run entered often
    // what the harvest saw seldom or never.
    uint32_t cold_entry = UINT32_MAX;
    // il.h Function::inlineScopes: the callee's function name, the call's
    // position, and the scope the call is in (0: the function's own code).
    struct InlineScope {
        std::string callee;
        uint32_t line = 0;
        uint32_t column = 0;
        uint32_t file = 0;
        uint32_t parent = 0;
    };
    std::vector<InlineScope> inline_scopes;
    std::vector<BronzeBlock> blocks;
    // A companion's interpreted function whose body is not converted yet
    // (TranslatorOptions::deferred_body builds it on first need): `blocks`
    // is empty, and `create_funcs` is what lowering reads of every body up
    // front, each CreateFunc's callee and parameter count, in body order.
    bool body_deferred = false;
    std::vector<std::pair<std::string, uint32_t>> create_funcs;
};

struct BronzeModuleAST {
    std::string name;
    // The IL module's source files (il::Module::sourceFiles), which an
    // instruction's `file` indexes: one function can hold code from several
    // of them (a program's merged top level), so the debug location of each
    // instruction names its own.
    std::vector<std::string> source_files;
    std::vector<BronzeFunction> functions;
    // The keys of the module's global-read cache, sorted and unique
    // (codegen::lowerToBrassAst): slot i of __bronze_global_cache caches the
    // global key global_read_keys[i] names, which is what lets a read load
    // its cell inline rather than call the slot's thunk. Empty for an AST
    // built without one, whose reads call the thunks.
    std::vector<uint32_t> global_read_keys;
};

const char* bronze_type_name(BronzeType t);
const char* bronze_op_name(BronzeOp op);

} // namespace il2mir
