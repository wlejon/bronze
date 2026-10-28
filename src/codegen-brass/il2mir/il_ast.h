#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <optional>

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
    ElemSetTyped,
    ElemGetTyped,
    PinGuard,
    CensusRecord,
    MathImul,
    SuperSet,
    MathUnary,
    DynamicImport,
    ModuleNamespace,
    ObjectKeys,
    ForInKeys,
    MethodDefComputed,
    AccessorDef,
    AccessorDefComputed,
    DefineOwnAttr,
    GetNewTarget,
    ImportMeta,
    SuperCallSpread,
    TemplateCached,
    TemplateObject,
    ArrayAppendHole,
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
    ConstructSpread,
    PrintSpread,
    PrintSpreadErr,
    KeepAlive,
    MethodIcIs,
    AccessorIcIs,
    Unknown
};

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
    bool raw_unbox = false;
    bool nullish_unbox = false;
    std::string callee_name;
    // MethodCall: the typed entry of `callee_name` the direct edge calls
    // (il.h `typedTarget`), or empty. The guard's identity stays `callee_name`.
    std::string typed_callee_name;
    std::string string_literal;

    uint32_t depth = 0;
    uint32_t index = 0;
    uint32_t param_count = 0;

    static constexpr uint32_t kNoStaticSlot = UINT32_MAX;
    static constexpr uint32_t kNoIcIndex = UINT32_MAX;
    uint32_t static_slot = kNoStaticSlot;
    uint32_t ic_index = kNoIcIndex;
    bool is_mono = false;
    bool is_fn_recv = false;
    // A read whose receiver inference typed by a class SOMEBODY EXTENDS (`this`
    // in a base-class method, a parameter whose callers named a base): more
    // than one shape is the expected case, so a way-0 miss scans the site's
    // other ways inline rather than calling the helper.
    bool is_family = false;
    // PropGet/PropSet with a `static_slot`: which guard licenses it (il.h
    // `staticCellIndex`, `familyLo`/`familySpan`). An identity site compares
    // the receiver's shape with its module cell `static_cell`; a family site
    // (`family_lo` not kNoFamily) asks the shape's stamp for a class id in
    // `family_lo .. family_lo + family_span`.
    static constexpr uint32_t kNoFamily = UINT32_MAX;
    uint32_t static_cell = 0;
    uint32_t family_lo = kNoFamily;
    uint32_t family_span = 0;

    BronzeBlockTarget target;
    BronzeBlockTarget else_target;

    uint32_t line = 0;
    uint32_t column = 0;
    // The index into BronzeModuleAST::source_files that `line` is in, or
    // UINT32_MAX for the module's own name (the instruction has no span).
    uint32_t file = UINT32_MAX;
    uint32_t env_hops = UINT32_MAX;
    // il::Instruction::inlineFrame: 0, or a 1-based index into the function's
    // `inline_frames`.
    uint32_t inline_frame = 0;
};

struct BronzeBlock {
    uint32_t id = 0;
    std::vector<std::pair<uint32_t, BronzeType>> params;
    uint32_t handler_id = UINT32_MAX;
    std::vector<BronzeInstruction> instructions;
};

struct BronzeFunction {
    std::string name;
    std::vector<std::pair<uint32_t, BronzeType>> params;
    std::vector<std::optional<int64_t>> param_const_ints;
    BronzeType return_type = BronzeType::Void;
    bool is_exported = false;
    // A coroutine body's BRONZE_ABI_CORO_* kind, or -1: the function runs on
    // a brass coroutine frame (its leading parameter) and is reached only
    // through `coro_create`.
    int32_t coro_kind = -1;
    bool is_coroutine_body() const { return coro_kind >= 0; }
    // The program's top level (`main` and its segments): run once.
    bool is_toplevel = false;
    bool is_deterministic_fp = false;
    // il::Function::inlineFrames, with the callee by its MIR function name and
    // the call's position as an instruction's (`file` the same index).
    struct InlineFrame {
        std::string callee;
        uint32_t line = 0;
        uint32_t column = 0;
        uint32_t file = UINT32_MAX;
        uint32_t parent = 0;
    };
    std::vector<InlineFrame> inline_frames;
    std::vector<BronzeBlock> blocks;
};

struct BronzeModuleAST {
    std::string name;
    // The IL module's source files (il::Module::sourceFiles), which an
    // instruction's `file` indexes: one function can hold code from several
    // of them (a program's merged top level), so the debug location of each
    // instruction names its own.
    std::vector<std::string> source_files;
    bool is_deterministic_fp = false;
    std::vector<BronzeFunction> functions;
};

const char* bronze_type_name(BronzeType t);
const char* bronze_op_name(BronzeOp op);

} // namespace il2mir
