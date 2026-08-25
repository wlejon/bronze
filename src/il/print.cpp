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
        case Op::ConstBigInt: return "const.bigint";
        case Op::Add: return "add";
        case Op::Sub: return "sub";
        case Op::Neg: return "neg";
        case Op::Mul: return "mul";
        case Op::Div: return "div";
        case Op::Mod: return "mod";
        case Op::Pow: return "pow";
        case Op::ToNumeric: return "to.numeric";
        case Op::NumericStep: return "numeric.step";
        case Op::ToInt32: return "to.int32";
        case Op::BitAnd: return "and";
        case Op::BitOr: return "or";
        case Op::BitXor: return "xor";
        case Op::Shl: return "shl";
        case Op::Shr: return "shr";
        case Op::UShr: return "ushr";
        case Op::BitNot: return "bitnot";
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
        case Op::ToStr: return "to.string";
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
        case Op::SuperSet: return "super.set";
        case Op::PropSet: return "prop.set";
        case Op::ElemGet: return "elem.get";
        case Op::ElemSet: return "elem.set";
        case Op::ElemGetTyped: return "elem.get.typed";
        case Op::ElemSetTyped: return "elem.set.typed";
        case Op::MathUnary: return "math.unary";
        case Op::DynamicCall: return "call.dynamic";
        case Op::FunctionRef: return "func.ref";
        case Op::Construct: return "new";
        case Op::CreateObject: return "create.object";
        case Op::CreateGeneratorObject: return "create.generator_object";
        case Op::CreateAsyncGeneratorObject: return "create.async_generator_object";
        case Op::CreateAsyncMachine: return "create.async_machine";
        case Op::AsyncStart: return "async.start";
        case Op::AsyncAwait: return "async.await";
        case Op::DynamicImport: return "dynamic_import";
        case Op::ModuleNamespace: return "module.namespace";
        case Op::ObjectKeys: return "object.keys";
        case Op::ForInKeys: return "forin.keys";
        case Op::MethodDef: return "method.def";
        case Op::MethodDefComputed: return "method.def.computed";
        case Op::AccessorDef: return "accessor.def";
        case Op::AccessorDefComputed: return "accessor.def.computed";
        case Op::GetNewTarget: return "get.new_target";
        case Op::ImportMeta: return "import.meta";
        case Op::SuperCall: return "call.super";
        case Op::SuperCallSpread: return "call.super.spread";
        case Op::TemplateCached: return "template.cached";
        case Op::TemplateObject: return "template.object";
        case Op::ArrayAppendHole: return "array.append.hole";
        case Op::PropDelete: return "prop.delete";
        case Op::ElemDelete: return "elem.delete";
        case Op::GlobalGet: return "global.get";
        case Op::ResolveName: return "name.resolve";
        case Op::ImmutableAssign: return "immutable.assign";
        case Op::PinGuard: return "pin.guard";
        case Op::ClassExtend: return "class.extend";
        case Op::PrivateNew: return "private.new";
        case Op::PrivateHas: return "private.has";
        case Op::PrivateGet: return "private.get";
        case Op::PrivateAdd: return "private.add";
        case Op::PrivateSet: return "private.set";
        case Op::PrivateMisuse: return "private.misuse";
        case Op::IterOpen: return "iter.open";
        case Op::AsyncIterOpen: return "async_iter.open";
        case Op::AsyncIterNext: return "async_iter.next";
        case Op::AsyncIterClose: return "async_iter.close";
        case Op::IterStep: return "iter.step";
        case Op::IterValue: return "iter.value";
        case Op::IterClose: return "iter.close";
        case Op::IterRest: return "iter.rest";
        case Op::IterDelegate: return "iter.delegate";
        case Op::PatternCheck: return "pattern.check";
        case Op::ArrayAppend: return "array.append";
        case Op::ArraySpread: return "array.spread";
        case Op::ObjectSpread: return "object.spread";
        case Op::ObjectRest: return "object.rest";
        case Op::DynamicCallSpread: return "call.dynamic.spread";
        case Op::MethodCall: return "method.call";
        case Op::MethodCallSpread: return "method.call.spread";
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
        case Op::PrintSpread: return "print.spread";
        case Op::PrintSpreadErr: return "print.spread.err";
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
        // The literal is a parse and an allocation, and a failure to allocate
        // is fatal rather than catchable.
        case Op::ConstBigInt:
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
        // Allocation and the environment: they can collect, and a failure to
        // allocate is fatal rather than catchable (there is no `RangeError:
        // out of memory` in bronze, and inventing one would let a program
        // continue past a heap that could not grow).
        case Op::CreateObject:
        case Op::CreateGeneratorObject:
        // The machine is allocation only; starting it and subscribing an
        // await are NOT here — the first runs the body and the second reads
        // `.then` off whatever was awaited, and either can reach user code.
        case Op::CreateAsyncMachine:
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
        // Minting a private name's table is an allocation and nothing else.
        // The other five private ops CAN throw and are not here — `#x in o`
        // included, because 13.10.1 step 6 refuses a non-object right operand
        // before it ever asks about the element.
        case Op::PrivateNew:
        case Op::GlobalGet:
        // console.log never runs user code: inspect does not call a getter.
        case Op::Print:
        case Op::PrintErr:
        case Op::PrintSpread:
        case Op::PrintSpreadErr:
        case Op::ExcTake:
        // Raw bounds-checked loads and stores on a proven view: an invalid
        // index is NaN or a skipped store, never a throw, and neither op
        // allocates or reaches user code.
        case Op::ElemGetTyped:
        case Op::ElemSetTyped:
        // A machine-number math kernel: no coercion ladder, no user code.
        case Op::MathUnary:
        // One aligned load from the module's own template-slot table, at an
        // address the compiler already proved in range.
        case Op::TemplateCached:
            return false;
        case Op::Add:
        // The rest of the arithmetic and bitwise family, for the same reason
        // `add` is here: each is TWO operations. On machine numbers it is one
        // instruction that cannot fail; on BOXED operands it is 13.15.3, which
        // reaches ToPrimitive, refuses a mixed BigInt/Number pair by TypeError
        // and divides by 0n by RangeError.
        case Op::Neg:
        case Op::Sub:
        case Op::Mul:
        case Op::Div:
        case Op::Mod:
        case Op::Pow:
        case Op::BitAnd:
        case Op::BitOr:
        case Op::BitXor:
        case Op::Shl:
        case Op::Shr:
        case Op::UShr:
            return inst.type == Type::Dynamic;
        // `bitnot` exists ONLY in its boxed form, so it has no cannot-throw
        // half to test for.
        case Op::BitNot:
            return true;
        case Op::Unbox:
            // `unbox.f64` is ToNumber (7.1.4) — generated code's only numeric
            // coercion — and ToNumber runs ToPrimitive on an object and throws
            // for a Symbol. `unbox.bool` is ToBoolean, which is total, and
            // `unbox.i32` reads a number it has already tested for; neither
            // calls anything. The RAW form calls nothing either: its operand is
            // proven a Number, so ToNumber is the identity and the emitted code
            // is one bitcast.
            // The NULLISH-WIDENED form calls nothing either: its operand is one
            // of a number, `null` or `undefined`, and ToNumber over those three
            // is a compare and two constants.
            return inst.type == Type::F64 && !inst.rawUnbox && !inst.nullishUnbox;
        case Op::ToInt32:
            // Same conversion under a different name: 7.1.6 step 1 is ToNumber,
            // so `{} | 0` and `sym | 0` reach user code and the TypeError. Only
            // a BOXED operand does — `boxType` carries the operand's type here,
            // and an f64 or i32 one is a machine conversion with no call in it,
            // which is what keeps a proven-numeric bitwise chain branch-free.
            return inst.boxType == Type::Dynamic;
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
                    out += "%";
                    out += std::to_string(inst.result);
                    out += ": ";
                    out += typeName(inst.type);
                    out += " = ";
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
                    // The DIGITS, not the key index: what the literal says is
                    // the whole content of the instruction, and an index would
                    // move whenever an unrelated key was registered ahead of
                    // it. Same argument `global.get` makes for its name.
                    case Op::ConstBigInt:
                        out += "const.bigint " +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "n";
                        break;
                    case Op::BitNot:
                        out += "bitnot %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::ToNumeric:
                        out += "to.numeric %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::NumericStep:
                        out += "numeric.step %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               (inst.immI32 > 0 ? ", +1" : ", -1");
                        break;
                    case Op::Unbox:
                        out += "unbox." + std::string(typeName(inst.type)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        // The raw form is a different instruction in every way
                        // that matters — it cannot throw and it emits one
                        // bitcast — so the canonical text names it.
                        if (inst.rawUnbox) out += ", raw";
                        if (inst.nullishUnbox) out += ", nullish";
                        break;
                    case Op::PropGet:
                        out += "prop.get %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", " + std::to_string(inst.icIndex);
                        // The site inference proved monomorphic: an identity
                        // claim about the receiver. Visible in the canonical
                        // text because it is what --infer-stats counts.
                        if (inst.icMonomorphic) out += ", mono";
                        // The stronger claim, and the one that changes emitted
                        // code: a proven class layout put this key at a
                        // constant instance slot, so the site carries a
                        // shape-compare-and-load fast path in front of the
                        // cache. Printed because a reader cannot otherwise tell
                        // the two forms apart in the IL.
                        if (inst.staticSlot != Instruction::kNoStaticSlot) {
                            out += ", slot " + std::to_string(inst.staticSlot);
                            // A family site names a RANGE of class ids and no
                            // cell; an identity site names its cell. Two
                            // spellings because they are two guards, and a
                            // reader of the IL has no other way to tell.
                            if (inst.familyLo != Instruction::kNoFamily) {
                                out += " family " + std::to_string(inst.familyLo) + ".." +
                                       std::to_string(inst.familyLo + inst.familySpan);
                            } else {
                                out += "@" + std::to_string(inst.staticCellIndex);
                            }
                        }
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
                    case Op::AccessorDefComputed:
                        out += "accessor.def.computed %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 3 ? inst.operands[3] : 0) +
                               ", " + (inst.immI32 ? "enumerable" : "non-enumerable");
                        break;
                    case Op::GetNewTarget:
                        out += "get.new_target";
                        break;
                    case Op::ImportMeta:
                        out += "import.meta \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    case Op::ArrayAppendHole:
                        out += "array.append.hole %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::TemplateCached:
                        out += "template.cached " + std::to_string(inst.immI32);
                        break;
                    case Op::TemplateObject:
                        out += "template.object %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::SuperCall: {
                        out += "call.super";
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
                    case Op::SuperCallSpread:
                        out += "call.super.spread %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
                        break;
                    case Op::SuperGet:
                        out += "super.get %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    case Op::SuperSet:
                        out += "super.set %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
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
                        // The read twin's note applies: the static slot is
                        // printed because it changes emitted code.
                        if (inst.staticSlot != Instruction::kNoStaticSlot) {
                            out += ", slot " + std::to_string(inst.staticSlot);
                            // A family site names a RANGE of class ids and no
                            // cell; an identity site names its cell. Two
                            // spellings because they are two guards, and a
                            // reader of the IL has no other way to tell.
                            if (inst.familyLo != Instruction::kNoFamily) {
                                out += " family " + std::to_string(inst.familyLo) + ".." +
                                       std::to_string(inst.familyLo + inst.familySpan);
                            } else {
                                out += "@" + std::to_string(inst.staticCellIndex);
                            }
                        }
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
                    case Op::MethodCall: {
                        out += "method.call";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        out += ", " + std::to_string(inst.keyIndex) + ", " +
                               std::to_string(inst.icIndex);
                        if (inst.icMonomorphic) out += ", mono";
                        if (inst.directTarget != Instruction::kNoDirectTarget) {
                            out += ", direct @" + std::to_string(inst.directTarget);
                        }
                        size_t argc = inst.operands.size() > 1 ? inst.operands.size() - 1 : 0;
                        out += ", " + std::to_string(argc);
                        for (size_t i = 1; i < inst.operands.size(); ++i) {
                            out += ", %" + std::to_string(inst.operands[i]);
                        }
                        break;
                    }
                    case Op::MethodCallSpread: {
                        out += "method.call.spread";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        out += ", " + std::to_string(inst.keyIndex) + ", " +
                               std::to_string(inst.icIndex);
                        if (inst.icMonomorphic) out += ", mono";
                        if (inst.operands.size() > 1) {
                            out += ", %" + std::to_string(inst.operands[1]);
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
                    case Op::ElemGetTyped:
                        out += "elem.get.typed %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::ElemSetTyped:
                        out += "elem.set.typed %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" + std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" + std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::MathUnary:
                        out += "math.unary %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::CreateObject:
                        out += "create.object";
                        break;
                    case Op::ModuleNamespace:
                    case Op::CreateGeneratorObject:
                    case Op::CreateAsyncGeneratorObject:
                    case Op::CreateAsyncMachine:
                    case Op::AsyncStart:
                    case Op::DynamicImport:
                    case Op::ForInKeys:
                    case Op::IterOpen:
                    case Op::AsyncIterOpen:
                    case Op::AsyncIterNext:
                    case Op::IterStep:
                    case Op::IterValue:
                    case Op::IterRest:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::IterDelegate:
                        out += "iter.delegate %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0);
                        break;
                    // The SUPPRESS flag, because it is the whole content of
                    // the instruction: a close on the throw path discards an
                    // error `return` raises and one on a `break` path does
                    // not (ECMA-262 7.4.9 step 6).
                    case Op::IterClose:
                    case Op::AsyncIterClose:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               (inst.immI32 == 0 ? "abrupt" : "suppress");
                        break;
                    case Op::ClassExtend:
                    case Op::AsyncAwait:
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
                    // The private-element family, which prints its operands
                    // and — for the two that can raise — the NAME the
                    // TypeError would carry, for the reason name.resolve prints
                    // one: which private name was reached IS the instruction.
                    case Op::PrivateNew:
                        out += "private.new";
                        break;
                    case Op::PrivateHas:
                    case Op::PrivateGet:
                    case Op::PrivateAdd:
                    case Op::PrivateSet: {
                        out += std::string(opName(inst.op));
                        for (size_t i = 0; i < inst.operands.size(); ++i) {
                            out += (i == 0 ? " %" : ", %") + std::to_string(inst.operands[i]);
                        }
                        if (inst.op != Op::PrivateAdd) {
                            out += ", \"" +
                                   (inst.keyIndex < module.keyConstants.size()
                                        ? module.keyConstants[inst.keyIndex]
                                        : std::string("?")) +
                                   "\"";
                        }
                        break;
                    }
                    case Op::PrivateMisuse:
                        out += "private.misuse \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\", " + std::to_string(inst.immI32);
                        break;
                    // The NAME for the same reason `global.get` prints one:
                    // which name the closed ladder could not settle IS the
                    // instruction. The trailing flag is 13.5.3's soft form —
                    // a bare `typeof`, which answers "undefined" for a miss
                    // where every other position raises.
                    case Op::ResolveName:
                        out += "name.resolve \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"" + (inst.immI32 != 0 ? ", soft" : "");
                        break;
                    // The NAME for the same reason name.resolve prints one:
                    // which binding was written to IS the instruction.
                    case Op::ImmutableAssign:
                        out += "immutable.assign \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    // The manifest LINE, because which promise this barrier is
                    // holding the program to IS the instruction, and the dump
                    // is what a reader bisects an unexpected throw with.
                    case Op::PinGuard: {
                        const char* shape = "number";
                        switch (static_cast<PinBarrier>(inst.immI32)) {
                            case PinBarrier::Number: shape = "number"; break;
                            case PinBarrier::NumberOrNullish: shape = "number-or-nullish"; break;
                            case PinBarrier::DenseArray: shape = "dense-array"; break;
                        }
                        out += "pin.guard %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + shape + ", \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    }
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
                    // The NAME, for the reason `name.resolve` prints one: which
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
                            // `env+N` before the arguments marks a direct call
                            // to a CLOSURE: operand 0 is the caller's record and
                            // N is the hop count to the callee's (il.h,
                            // `callEnvHops`). It is what distinguishes this
                            // instruction from one the verifier would reject.
                            out += "(";
                            for (size_t i = 0; i < inst.operands.size(); ++i) {
                                if (i > 0) out += ", ";
                                if (i == 0 && inst.callEnvHops != Instruction::kNoEnvHops) {
                                    out += "env+" + std::to_string(inst.callEnvHops) + " ";
                                }
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
