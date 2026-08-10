#pragma once
#include <cstdint>
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
    Add,        // a = add b, c        (numeric, operands same type)
    Sub,
    Mul,
    Div,
    CmpLt,      // a: bool = cmp.lt b, c
    CmpGt,
    CmpEq,
    Ret,        // ret [a]
    Call,       // a = call <funcRef>(args...)
    Box,        // a = box.<type> b
    Unbox,      // a = unbox.<type> b
    PropGet,    // a = prop.get b, <key_const_index>, <ic_site_index>
    PropSet,    // prop.set b, <key_const_index>, c, <ic_site_index>
    DynamicCall,// a = call.dynamic callee, thisArg, argc, argv
};
const char* opName(Op op);

using ValueId = uint32_t;
inline constexpr ValueId kNoValue = UINT32_MAX;

struct Instruction {
    Op op;
    Type type = Type::Void;          // result type (Void: no result)
    ValueId result = kNoValue;
    std::vector<ValueId> operands;
    double immF64 = 0;               // ConstF64
    int32_t immI32 = 0;              // ConstI32
    uint32_t calleeIndex = 0;        // Call: index into Module::functions
    Type boxType = Type::Void;       // Box: input type being boxed (F64, I32, Bool, Str)
    uint32_t keyIndex = 0;           // PropGet/PropSet: key constant index
    uint32_t icIndex = 0;            // PropGet/PropSet: IC site index
};

struct Param {
    std::string name;
    Type type;
};

struct Function {
    std::string name;
    std::vector<Param> params;
    Type returnType = Type::Void;
    bool isExported = false;
    // Single block for now; the block structure arrives with control flow.
    std::vector<Instruction> body;
    uint32_t valueCount = 0;  // number of ValueIds in use (params first)
};

struct Module {
    std::string name;
    std::vector<Function> functions;
};

}  // namespace bronze::il
