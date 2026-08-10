#include "il/print.h"

#include <charconv>

namespace bronze::il {

const char* typeName(Type t) {
    switch (t) {
        case Type::Void: return "void";
        case Type::Bool: return "bool";
        case Type::I32: return "i32";
        case Type::F64: return "f64";
        case Type::Str: return "str";
        case Type::Dynamic: return "dynamic";
    }
    return "?";
}

const char* opName(Op op) {
    switch (op) {
        case Op::ConstF64: return "const.f64";
        case Op::ConstI32: return "const.i32";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::CmpLt: return "cmp.lt";
        case Op::CmpGt: return "cmp.gt";
        case Op::CmpEq: return "cmp.eq";
        case Op::Ret: return "ret";
        case Op::Call: return "call";
    }
    return "?";
}

// Shortest round-trippable float formatting (std::to_chars is locale-free).
static std::string formatF64(double v) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

std::string print(const Module& module) {
    std::string out = "module " + module.name + "\n";
    for (const auto& fn : module.functions) {
        out += "\nfunc " + fn.name + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i > 0) out += ", ";
            out += "%" + std::to_string(i) + ": " + typeName(fn.params[i].type);
        }
        out += ") -> ";
        out += typeName(fn.returnType);
        if (fn.isExported) out += " export";
        out += " {\n";
        for (const auto& inst : fn.body) {
            out += "  ";
            if (inst.result != kNoValue) {
                out += "%" + std::to_string(inst.result) + ": " + typeName(inst.type) + " = ";
            }
            out += opName(inst.op);
            switch (inst.op) {
                case Op::ConstF64: out += " " + formatF64(inst.immF64); break;
                case Op::ConstI32: out += " " + std::to_string(inst.immI32); break;
                case Op::Call:
                    out += " @" + module.functions[inst.calleeIndex].name;
                    break;
                default: break;
            }
            if (inst.op == Op::Call) {
                out += "(";
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += "%" + std::to_string(inst.operands[i]);
                }
                out += ")";
            } else {
                for (size_t i = 0; i < inst.operands.size(); ++i) {
                    out += i == 0 ? " " : ", ";
                    out += "%" + std::to_string(inst.operands[i]);
                }
            }
            out += "\n";
        }
        out += "}\n";
    }
    return out;
}

}  // namespace bronze::il
