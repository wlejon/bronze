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
        case Op::CmpLe: return "cmp.le";
        case Op::CmpGe: return "cmp.ge";
        case Op::CmpEq: return "cmp.eq";
        case Op::CmpNe: return "cmp.ne";
        case Op::NumTruthy: return "num.truthy";
        case Op::StrictEq: return "strict.eq";
        case Op::LooseEq: return "loose.eq";
        case Op::RelLt: return "rel.lt";
        case Op::RelGt: return "rel.gt";
        case Op::RelLe: return "rel.le";
        case Op::RelGe: return "rel.ge";
        case Op::TypeOf: return "typeof";
        case Op::InstanceOf: return "instanceof";
        case Op::In: return "in";
        case Op::IsNullish: return "is.nullish";
        case Op::Ret: return "ret";
        case Op::Throw: return "throw";
        case Op::ExcTake: return "exc.take";
        case Op::Jump: return "jump";
        case Op::Branch: return "br";
        case Op::Call: return "call";
        case Op::Box: return "box";
        case Op::Unbox: return "unbox";
        case Op::PropGet: return "prop.get";
        case Op::SuperGet: return "super.get";
        case Op::PropSet: return "prop.set";
        case Op::ElemGet: return "elem.get";
        case Op::ElemSet: return "elem.set";
        case Op::DynamicCall: return "call.dynamic";
        case Op::FunctionRef: return "func.ref";
        case Op::Construct: return "new";
        case Op::CreateObject: return "create.object";
        case Op::CreateGeneratorObject: return "create.generator_object";
        case Op::ModuleNamespace: return "module.namespace";
        case Op::ObjectKeys: return "object.keys";
        case Op::ForInKeys: return "forin.keys";
        case Op::MethodDef: return "method.def";
        case Op::MethodDefComputed: return "method.def.computed";
        case Op::AccessorDef: return "accessor.def";
        case Op::PropDelete: return "prop.delete";
        case Op::ElemDelete: return "elem.delete";
        case Op::GlobalGet: return "global.get";
        case Op::RefError: return "ref.error";
        case Op::ClassExtend: return "class.extend";
        case Op::IterOpen: return "iter.open";
        case Op::IterStep: return "iter.step";
        case Op::IterValue: return "iter.value";
        case Op::IterClose: return "iter.close";
        case Op::IterRest: return "iter.rest";
        case Op::PatternCheck: return "pattern.check";
        case Op::ArrayAppend: return "array.append";
        case Op::ArraySpread: return "array.spread";
        case Op::ObjectSpread: return "object.spread";
        case Op::ObjectRest: return "object.rest";
        case Op::DynamicCallSpread: return "call.dynamic.spread";
        case Op::ConstructSpread: return "new.spread";
        case Op::CreateArray: return "create.array";
        case Op::CreateFunction: return "create.func";
        case Op::EnvCreate: return "env.create";
        case Op::EnvGet: return "env.get";
        case Op::EnvSet: return "env.set";
        case Op::EnvInitTdz: return "env.init.tdz";
        case Op::EnvGetTdz: return "env.get.tdz";
        case Op::ModuleEnvSet: return "module.env.set";
        case Op::ModuleEnvGet: return "module.env.get";
        case Op::Print: return "print";
        case Op::PrintErr: return "print.err";
    }
    return "?";
}

bool isTerminator(Op op) {
    return op == Op::Ret || op == Op::Jump || op == Op::Branch || op == Op::Throw;
}

// Can this instruction leave an exception pending? The backend emits one cell
// test after every instruction that answers yes, so the list is written the
// safe way round: the cases below are the ones that provably cannot, and
// everything else does. An op added tomorrow gets a redundant branch rather
// than a missed unwind.
//
// It is a property of the INSTRUCTION and not of the op because `add` is two
// operations: f64 arithmetic, which cannot throw, and `bronze_dynamic_add`,
// which reaches ToPrimitive.
bool canThrow(const Instruction& inst) {
    switch (inst.op) {
        // Constants and pure arithmetic: no call at all.
        case Op::ConstF64:
        case Op::ConstI32:
        case Op::ConstBool:
        case Op::ConstUndefined:
        case Op::ConstNull:
        case Op::Neg:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Pow:
        case Op::ToInt32:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
        case Op::CmpLt:
        case Op::CmpGt:
        // The ordered compares are machine instructions on two numbers, like
        // the two above them. Their boxed siblings — rel.lt and friends — are
        // NOT here: those reach ToPrimitive.
        case Op::CmpLe:
        case Op::CmpGe:
        case Op::CmpEq:
        case Op::CmpNe:
        case Op::NumTruthy:
        case Op::StrictEq:
        case Op::IsNullish:
        case Op::TypeOf:
        case Op::Box:
        case Op::Unbox:
        // Allocation and the environment: they can collect, and a failure to
        // allocate is fatal rather than catchable (there is no `RangeError:
        // out of memory` in bronze, and inventing one would let a program
        // continue past a heap that could not grow).
        case Op::CreateObject:
        case Op::CreateGeneratorObject:
        case Op::ModuleNamespace:
        case Op::CreateArray:
        case Op::CreateFunction:
        case Op::FunctionRef:
        case Op::EnvCreate:
        case Op::EnvGet:
        case Op::EnvSet:
        case Op::EnvInitTdz:
        case Op::ModuleEnvSet:
        case Op::ModuleEnvGet:
        case Op::GlobalGet:
        // console.log never runs user code: inspect does not call a getter.
        case Op::Print:
        case Op::PrintErr:
        case Op::ExcTake:
            return false;
        case Op::Add:
            // Dynamic `+` is bronze_dynamic_add, which is ToPrimitive.
            return inst.type == Type::Dynamic;
        default:
            return !isTerminator(inst.op);
    }
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
            // Only when there is one, so the dump of a program with no `try`
            // in it is byte-identical to what it was before exceptions
            // existed — which is what keeps every pinned IL ratchet valid.
            if (block.handler != kNoBlock) {
                out += " handler b" + std::to_string(block.handler);
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
                    // The key index, and no cache slot: a class body runs
                    // once, so a method definition has no repeat to cache
                    // against and is handed no IC site.
                    case Op::MethodDef:
                        out += "method.def %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    // The KEY as a value, where `method.def` prints a constant
                    // index: there is no constant, and printing one would name
                    // a key this instruction does not have.
                    case Op::MethodDefComputed:
                        out += "method.def.computed %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
                        break;
                    // Both halves are printed even when one is undefined:
                    // which half the source wrote is exactly what this op
                    // carries, and a form that dropped the absent one would
                    // print `get x` and `set x` identically.
                    case Op::AccessorDef:
                        out += "accessor.def %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0) +
                               ", " + (inst.immI32 ? "enumerable" : "non-enumerable");
                        break;
                    case Op::SuperGet:
                        out += "super.get %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    case Op::PropDelete:
                        out += "prop.delete %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", " +
                               std::to_string(inst.immI32);
                        break;
                    case Op::PropSet:
                        out += "prop.set %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", " + std::to_string(inst.icIndex) + ", " +
                               std::to_string(inst.immI32);
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
                               ", %" + std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::CreateObject:
                        out += "create.object";
                        break;
                    case Op::CreateGeneratorObject:
                        out += "create.generator_object";
                        break;
                    case Op::ModuleNamespace:
                    case Op::ForInKeys:
                    case Op::IterOpen:
                    case Op::IterStep:
                    case Op::IterValue:
                    case Op::IterRest:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    // The SUPPRESS flag, because it is the whole content of
                    // the instruction: a close on the throw path discards an
                    // error `return` raises and one on a `break` path does
                    // not (ECMA-262 7.4.9 step 6).
                    case Op::IterClose:
                        out += "iter.close %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               (inst.immI32 == 0 ? "abrupt" : "suppress");
                        break;
                    case Op::ClassExtend:
                    case Op::ArrayAppend:
                    case Op::ArraySpread:
                    case Op::ObjectSpread:
                    case Op::ObjectRest:
                    case Op::ConstructSpread:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    // The KIND, because which pattern asked for the check is
                    // the whole content of the instruction: it decides which
                    // diagnostic a bad source produces.
                    case Op::PatternCheck:
                        out += "pattern.check %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               (inst.immI32 == 0 ? "array" : "object");
                        break;
                    case Op::DynamicCallSpread:
                        out += "call.dynamic.spread %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
                        break;
                    // The NAME, not the key index: which global is being
                    // resolved is the whole content of the instruction, and
                    // an index would make the dump move whenever an
                    // unrelated property key was added ahead of it.
                    // The NAME for the same reason `global.get` prints one:
                    // which name failed to resolve IS the instruction.
                    case Op::RefError:
                        out += "ref.error \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
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
                    case Op::EnvInitTdz:
                        out += "env.init.tdz %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               std::to_string(inst.envDepth) + ", " +
                               std::to_string(inst.envIndex);
                        break;
                    // The NAME, for the reason `ref.error` prints one: which
                    // binding was read inside its dead zone IS the instruction.
                    case Op::EnvGetTdz:
                        out += "env.get.tdz %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               std::to_string(inst.envDepth) + ", " +
                               std::to_string(inst.envIndex) + ", \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    case Op::ModuleEnvSet:
                        out += "module.env.set %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::ModuleEnvGet:
                        out += "module.env.get";
                        break;
                    case Op::Print:
                    case Op::PrintErr:
                        // Every argument, because console.log takes any number
                        // of them and the dump is what a reader bisects with.
                        out += opName(inst.op);
                        for (size_t i = 0; i < inst.operands.size(); ++i) {
                            out += (i == 0 ? " %" : ", %") + std::to_string(inst.operands[i]);
                        }
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
