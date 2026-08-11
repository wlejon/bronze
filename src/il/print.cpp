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
        case Op::ConstBool: return "const.bool";
        case Op::ConstUndefined: return "const.undefined";
        case Op::ConstNull: return "const.null";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Neg: return "neg";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Mod: return "mod";
        case Op::Pow: return "pow";
        case Op::ToInt32: return "to.int32";
        case Op::BitAnd: return "and";
        case Op::BitOr: return "or";
        case Op::BitXor: return "xor";
        case Op::Shl: return "shl";
        case Op::Shr: return "shr";
        case Op::UShr: return "ushr";
        case Op::CmpLt: return "cmp.lt";
        case Op::CmpGt: return "cmp.gt";
        case Op::CmpEq: return "cmp.eq";
        case Op::CmpNe: return "cmp.ne";
        case Op::NumTruthy: return "num.truthy";
        case Op::StrictEq: return "strict.eq";
        case Op::LooseEq: return "loose.eq";
        case Op::TypeOf: return "typeof";
        case Op::InstanceOf: return "instanceof";
        case Op::In: return "in";
        case Op::IsNullish: return "is.nullish";
        case Op::Ret: return "ret";
        case Op::Jump: return "jump";
        case Op::Branch: return "br";
        case Op::Call: return "call";
        case Op::Box: return "box";
        case Op::Unbox: return "unbox";
        case Op::PropGet: return "prop.get";
        case Op::PropSet: return "prop.set";
        case Op::ElemGet: return "elem.get";
        case Op::ElemSet: return "elem.set";
        case Op::DynamicCall: return "call.dynamic";
        case Op::FunctionRef: return "func.ref";
        case Op::Construct: return "new";
        case Op::CreateObject: return "create.object";
        case Op::ObjectKeys: return "object.keys";
        case Op::GlobalGet: return "global.get";
        case Op::ClassExtend: return "class.extend";
        case Op::IterLength: return "iter.length";
        case Op::IterAt: return "iter.at";
        case Op::IterAdvance: return "iter.advance";
        case Op::CreateArray: return "create.array";
        case Op::CreateFunction: return "create.func";
        case Op::EnvCreate: return "env.create";
        case Op::EnvGet: return "env.get";
        case Op::EnvSet: return "env.set";
        case Op::CreateArrayBuffer: return "create.arraybuffer";
        case Op::CreateFloat32Array: return "create.f32array";
        case Op::Print: return "print";
    }
    return "?";
}

bool isTerminator(Op op) {
    return op == Op::Ret || op == Op::Jump || op == Op::Branch;
}

// Shortest round-trippable float formatting (std::to_chars is locale-free).
static std::string formatF64(double v) {
    char buf[32];
    const auto res = std::to_chars(buf, buf + sizeof(buf), v);
    return std::string(buf, res.ptr);
}

static std::string formatBlockTarget(const BlockTarget& target) {
    std::string out = "b" + std::to_string(target.block);
    if (!target.args.empty()) {
        out += "(";
        for (size_t i = 0; i < target.args.size(); ++i) {
            if (i > 0) out += ", ";
            out += "%" + std::to_string(target.args[i]);
        }
        out += ")";
    }
    return out;
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
        for (const auto& block : fn.blocks) {
            out += "  b" + std::to_string(block.id);
            if (!block.params.empty()) {
                out += "(";
                for (size_t i = 0; i < block.params.size(); ++i) {
                    if (i > 0) out += ", ";
                    out += "%" + std::to_string(block.params[i].id) + ": " + typeName(block.params[i].type);
                }
                out += ")";
            }
            out += ":\n";

            for (const auto& inst : block.instructions) {
                out += "    ";
                if (inst.result != kNoValue) {
                    out += "%" + std::to_string(inst.result) + ": " + typeName(inst.type) + " = ";
                }
                switch (inst.op) {
                    case Op::Jump:
                        out += "jump " + formatBlockTarget(inst.target);
                        break;
                    case Op::Branch:
                        out += "br %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + formatBlockTarget(inst.target) + ", " + formatBlockTarget(inst.elseTarget);
                        break;
                    case Op::Ret:
                        out += "ret";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        break;
                    case Op::Box:
                        // A str box with no operand takes its payload from the
                        // module key-constant table, not a ValueId.
                        out += "box." + std::string(typeName(inst.boxType));
                        if (inst.operands.empty()) {
                            out += " k" + std::to_string(inst.keyIndex);
                        } else {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        break;
                    case Op::Neg:
                        out += "neg %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::Unbox:
                        out += "unbox." + std::string(typeName(inst.type)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::PropGet:
                        out += "prop.get %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", " + std::to_string(inst.icIndex);
                        // The site inference proved monomorphic, which is
                        // what licenses the inlined cache check in the
                        // backend — visible in the canonical text because
                        // it changes the code that gets emitted.
                        if (inst.icMonomorphic) out += ", mono";
                        break;
                    case Op::PropSet:
                        out += "prop.set %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", " + std::to_string(inst.icIndex);
                        break;
                    case Op::DynamicCall: {
                        out += "call.dynamic";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                            if (inst.operands.size() > 1) {
                                out += ", %" + std::to_string(inst.operands[1]);
                            }
                        }
                        size_t argc = inst.operands.size() >= 2 ? inst.operands.size() - 2 : 0;
                        out += ", " + std::to_string(argc);
                        for (size_t i = 2; i < inst.operands.size(); ++i) {
                            out += ", %" + std::to_string(inst.operands[i]);
                        }
                        break;
                    }
                    case Op::FunctionRef:
                        out += "func.ref @" + (inst.calleeIndex < module.functions.size()
                                                   ? module.functions[inst.calleeIndex].name
                                                   : "?");
                        break;
                    case Op::Construct: {
                        out += "new";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        size_t nargs = inst.operands.empty() ? 0 : inst.operands.size() - 1;
                        out += ", " + std::to_string(nargs);
                        for (size_t i = 1; i < inst.operands.size(); ++i) {
                            out += ", %" + std::to_string(inst.operands[i]);
                        }
                        break;
                    }
                    case Op::ElemGet:
                        out += "elem.get %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    case Op::ElemSet:
                        out += "elem.set %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" + std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
                        break;
                    case Op::CreateArrayBuffer:
                        out += "create.arraybuffer %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::CreateFloat32Array:
                        out += "create.f32array %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::CreateObject:
                        out += "create.object";
                        break;
                    case Op::IterLength:
                        out += "iter.length %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::ClassExtend:
                    case Op::IterAt:
                    case Op::IterAdvance:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    // The NAME, not the key index: which global is being
                    // resolved is the whole content of the instruction, and
                    // an index would make the dump move whenever an
                    // unrelated property key was added ahead of it.
                    case Op::GlobalGet:
                        out += "global.get \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    case Op::CreateArray:
                        out += "create.array " + std::to_string(inst.immI32);
                        break;
                    case Op::CreateFunction:
                        out += "create.func @" + (inst.calleeIndex < module.functions.size() ? module.functions[inst.calleeIndex].name : "?") +
                               ", " + std::to_string(inst.immI32) + ", %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::EnvCreate:
                        out += "env.create %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::EnvGet:
                        out += "env.get %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.envDepth) + ", " + std::to_string(inst.envIndex);
                        break;
                    case Op::EnvSet:
                        out += "env.set %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.envDepth) + ", " + std::to_string(inst.envIndex) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    case Op::Print:
                        out += "print %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    default:
                        out += opName(inst.op);
                        switch (inst.op) {
                            case Op::ConstF64: out += " " + formatF64(inst.immF64); break;
                            case Op::ConstI32: out += " " + std::to_string(inst.immI32); break;
                            case Op::ConstBool: out += " " + std::string(inst.immI32 ? "true" : "false"); break;
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
                        break;
                }
                out += "\n";
            }
        }
        out += "}\n";
    }
    return out;
}

}  // namespace bronze::il
