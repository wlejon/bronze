#include "il/print.h"

#include <charconv>

#include "il/feedback_site.h"
#include "il/il_pin.h"

namespace bronze::il {

// `typeName` and `opName` are in `print_names.cpp`.

// `isTerminator`, `canThrow` and `canCollect` are in `effects.cpp`: what an
// instruction does is a different subject from what it is called.

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

std::string print(const Module& module, const std::vector<std::string>& fnNames) {
    auto getFnName = [&](size_t idx) -> std::string {
        if (idx < fnNames.size() && !fnNames[idx].empty()) return fnNames[idx];
        return idx < module.functions.size() ? module.functions[idx].name : "?";
    };
    std::string out = "module " + module.name + "\n";
    // A type-feedback build registers through the module's census table
    // (abi/bronze_feedback_site.h): its rows are the output path's marker and
    // the method-name links, which are summarized rather than listed — the
    // sites themselves are the `feedback.record` instructions.
    size_t feedbackRows = 0;
    for (const auto& site : module.censusSites) {
        if ((site.info & BRONZE_FEEDBACK_FLAG) != 0) ++feedbackRows;
    }
    if (feedbackRows != 0) {
        out += "\nfeedback " + module.censusOutPath + " (" + std::to_string(feedbackRows) +
               " registration rows)\n";
    }
    // The native import table and the declarations that reach it, before any
    // body: a call to `@__bronze_native_3` in a dump is only readable next to
    // the line saying what slot 3 is.
    if (!module.nativeImports.empty()) {
        out += "native-imports {\n";
        for (size_t i = 0; i < module.nativeImports.size(); ++i) {
            const auto& imp = module.nativeImports[i];
            out += "  [" + std::to_string(i) + "] \"" + imp.name + "\"";
            if (!imp.signature.empty()) out += " " + imp.signature;
            if (imp.bufferReturnKind != UINT32_MAX) {
                out += " buffer-return kind=" + std::to_string(imp.bufferReturnKind);
            }
            out += "\n";
        }
        out += "}\n";
    }
    // The layout cells before any body, for the reason the import table is:
    // a `shape.cell 3` is only readable next to what cell 3 is.
    if (!module.shapeCells.empty()) {
        out += "shape-cells {\n";
        for (size_t i = 0; i < module.shapeCells.size(); ++i) {
            out += "  [" + std::to_string(i) + "] \"" + module.shapeCells[i].key + "\"\n";
        }
        out += "}\n";
    }
    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        const auto& fn = module.functions[fnIdx];
        if (!fn.isExternal) continue;
        out += "extern func " + getFnName(fnIdx) + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i > 0) out += ", ";
            out += typeName(fn.params[i].type);
        }
        out += ") -> " + std::string(typeName(fn.returnType)) + "\n";
    }
    for (size_t fnIdx = 0; fnIdx < module.functions.size(); ++fnIdx) {
        const auto& fn = module.functions[fnIdx];
        if (fn.blocks.empty()) continue;
        out += "\nfunc " + getFnName(fnIdx) + "(";
        for (size_t i = 0; i < fn.params.size(); ++i) {
            if (i > 0) out += ", ";
            out += "%" + std::to_string(i) + ": " + typeName(fn.params[i].type);
        }
        out += ") -> ";
        out += typeName(fn.returnType);
        if (fn.isExported) out += " export";
        if (fn.isSpeculated()) out += " speculated @" + getFnName(fn.speculatedFrom);
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
            if (block.cold) out += " [cold]";
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
                    // `concat.append` and `concat.end` take the default arm —
                    // operands and nothing else. `concat.begin` carries the
                    // remaining-operand count, which is the one thing a reader
                    // cannot recover from the surrounding instructions.
                    case Op::ConcatBegin:
                        out += "concat.begin %" +
                               std::to_string(inst.operands.size() > 0 ? inst.operands[0] : 0) +
                               ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::Unbox:
                        out += "unbox." + std::string(typeName(inst.type)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::PropGet:
                        out += "prop.get %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", " + std::to_string(inst.icIndex);
                        if (inst.immI32 & kPropGetArrayLength) out += ", array-length";
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
                    // The mask spelled out, in 6.2.6.5's own field order, and
                    // only for the fields the descriptor HAS: "absent" and
                    // "present and false" are different programs (10.1.6.3
                    // step 4), so a dump that printed all four would hide the
                    // one distinction this instruction exists to carry. `{}`
                    // prints as an empty list, which is what a generic
                    // descriptor is.
                    case Op::DefineOwnAttr: {
                        out += "define.own.attr %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.keyIndex) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0) +
                               ", {";
                        const auto mask = static_cast<uint32_t>(inst.immI32);
                        std::string fields;
                        auto field = [&](uint32_t hasBit, uint32_t valueBit, const char* name) {
                            if ((mask & hasBit) == 0) return;
                            if (!fields.empty()) fields += ",";
                            fields += name;
                            if (valueBit != 0) {
                                fields += (mask & valueBit) != 0 ? "=true" : "=false";
                            }
                        };
                        field(BRONZE_ABI_DESC_HAS_VALUE, 0, "value");
                        field(BRONZE_ABI_DESC_HAS_WRITABLE, BRONZE_ABI_DESC_WRITABLE, "writable");
                        field(BRONZE_ABI_DESC_HAS_ENUMERABLE, BRONZE_ABI_DESC_ENUMERABLE,
                              "enumerable");
                        field(BRONZE_ABI_DESC_HAS_CONFIGURABLE, BRONZE_ABI_DESC_CONFIGURABLE,
                              "configurable");
                        out += fields + "}";
                        if ((mask & BRONZE_ABI_DESC_FROM_DEFINE_PROPERTY) != 0) out += " single";
                        break;
                    }
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
                    case Op::TemplateCached:
                        out += "template.cached " + std::to_string(inst.immI32);
                        break;
                    // The claim's index bound is its content beside the receiver.
                    case Op::IsDenseArray:
                    case Op::IsTypedArray:
                        out += std::string(opName(inst.op)) + " %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", " +
                               std::to_string(inst.immI32);
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
                               std::to_string(inst.operands.size() > 2 ? inst.operands[2] : 0) +
                               ", " + std::to_string(inst.immI32);
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
                    case Op::MethodCall: {
                        out += "method.call";
                        if (!inst.operands.empty()) {
                            out += " %" + std::to_string(inst.operands[0]);
                        }
                        out += ", " + std::to_string(inst.keyIndex) + ", " +
                               std::to_string(inst.icIndex);
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
                        if (inst.operands.size() > 1) {
                            out += ", %" + std::to_string(inst.operands[1]);
                        }
                        break;
                    }
                    case Op::FunctionRef:
                        out += "func.ref @" + getFnName(inst.calleeIndex);
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
                        if (inst.immI32 != 0) out += ", " + std::to_string(inst.immI32);
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
                    case Op::ModuleNamespace:
                        out += "module.namespace %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    case Op::DynamicImport:
                        out += "dynamic_import %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", \"" +
                               (inst.keyIndex < module.keyConstants.size()
                                    ? module.keyConstants[inst.keyIndex]
                                    : std::string("?")) +
                               "\"";
                        break;
                    case Op::CoroStart: {
                        static const char* const kKinds[] = {"generator", "async",
                                                             "async_generator"};
                        out += "coro.start ";
                        out += inst.immI32 >= 0 && inst.immI32 < 3 ? kKinds[inst.immI32] : "?";
                        out += " @" + (inst.calleeIndex < module.functions.size()
                                           ? module.functions[inst.calleeIndex].name
                                           : std::string("?")) +
                               "(";
                        for (size_t i = 0; i < inst.operands.size(); ++i) {
                            if (i) out += ", ";
                            out += "%" + std::to_string(inst.operands[i]);
                        }
                        out += ")";
                        break;
                    }
                    case Op::CoroSuspend: {
                        static const char* const kKinds[] = {"start", "yield", "await",
                                                             "delegate"};
                        out += "coro.suspend ";
                        out += inst.immI32 >= 0 && inst.immI32 < 4 ? kKinds[inst.immI32] : "?";
                        out += " %" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]);
                        break;
                    }
                    case Op::CoroMode:
                        out += "coro.mode";
                        break;
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
                    case Op::ArrayAppend:
                    case Op::ArraySpread:
                    case Op::ObjectSpread:
                    case Op::ObjectRest:
                    case Op::ObjectLitProto:
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
                    // The site's kind and ordinal, which is what a profile
                    // row is filed under (docs/type-feedback.md), and the
                    // function key the run will file it in.
                    case Op::FeedbackRecord: {
                        const uint32_t info = static_cast<uint32_t>(inst.immI32);
                        out += "feedback.record ";
                        if (!inst.operands.empty()) out += "%" + std::to_string(inst.operands[0]) + ", ";
                        out += std::string(feedbackKindName(static_cast<FeedbackKind>(
                                   BRONZE_FEEDBACK_INFO_KIND(info)))) +
                               " #" + std::to_string(BRONZE_FEEDBACK_INFO_ORDINAL(info)) + ", \"" +
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
                        out += "create.func @" + getFnName(inst.calleeIndex) +
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
                    // The resume id, then the state in its fixed order: the
                    // order IS the contract a `leave` matches against.
                    case Op::ResumePoint:
                        out += "resume.point " + std::to_string(inst.immI32);
                        for (ValueId v : inst.operands) out += ", %" + std::to_string(v);
                        break;
                    case Op::Leave:
                    case Op::ResumeCall:
                        out += std::string(opName(inst.op)) + " @" + getFnName(inst.calleeIndex) +
                               ", " + std::to_string(inst.immI32);
                        for (ValueId v : inst.operands) out += ", %" + std::to_string(v);
                        break;
                    // The cell's KEY beside its number: which shape a test
                    // compares against is the content of the test.
                    case Op::ShapeCell:
                        out += "shape.cell " + std::to_string(inst.immI32);
                        if (inst.immI32 >= 0 &&
                            static_cast<size_t>(inst.immI32) < module.shapeCells.size()) {
                            out += " \"" + module.shapeCells[inst.immI32].key + "\"";
                        }
                        break;
                    case Op::ObjectLiteral: {
                        out += "object.literal " + std::to_string(inst.immI32) + " {";
                        if (inst.immI32 >= 0 && static_cast<size_t>(inst.immI32) < module.objectLiterals.size()) {
                            const auto& keys = module.objectLiterals[inst.immI32].keys;
                            for (size_t k = 0; k < keys.size(); ++k) {
                                out += (k ? "," : "") +
                                       (keys[k] < module.keyConstants.size() ? module.keyConstants[keys[k]]
                                                                              : std::string("?"));
                            }
                        }
                        out += "}";
                        for (ValueId v : inst.operands) out += ", %" + std::to_string(v);
                        break;
                    }
                    case Op::MemberCell:
                    case Op::MemberPublish: {
                        out += std::string(opName(inst.op));
                        for (ValueId v : inst.operands) out += " %" + std::to_string(v) + ",";
                        out += " " + std::to_string(inst.immI32);
                        if (inst.immI32 >= 0 && static_cast<size_t>(inst.immI32) < module.memberCells.size()) {
                            const Module::MemberCell& c = module.memberCells[inst.immI32];
                            static const char* const kKinds[] = {"method", "getter", "setter"};
                            out += std::string(" ") + kKinds[static_cast<int>(c.kind)] + " \"" +
                                   (c.keyIndex < module.keyConstants.size() ? module.keyConstants[c.keyIndex]
                                                                            : std::string("?")) +
                                   "\" @" + getFnName(c.callee);
                        }
                        break;
                    }
                    case Op::ProtoCell:
                    case Op::ProtoPublish: {
                        out += std::string(opName(inst.op)) + " " + std::to_string(inst.immI32);
                        if (inst.immI32 >= 0 && static_cast<size_t>(inst.immI32) < module.protoCells.size()) {
                            const Module::ProtoCell& c = module.protoCells[inst.immI32];
                            out += std::string(" add \"") +
                                   (c.keyIndex < module.keyConstants.size() ? module.keyConstants[c.keyIndex]
                                                                            : std::string("?")) +
                                   "\" on \"" +
                                   (c.fromCell < module.shapeCells.size() ? module.shapeCells[c.fromCell].key
                                                                           : std::string("?")) +
                                   "\"";
                        }
                        break;
                    }
                    case Op::SlotLoad:
                        out += "slot.load %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.immI32);
                        break;
                    case Op::SlotStore:
                        out += "slot.store %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", " + std::to_string(inst.immI32) + ", %" +
                               std::to_string(inst.operands.size() > 1 ? inst.operands[1] : 0);
                        break;
                    case Op::TypedLoad:
                    case Op::TypedStore:
                        out += std::string(opName(inst.op));
                        for (size_t k = 0; k < inst.operands.size(); ++k) {
                            out += (k == 0 ? " %" : ", %") + std::to_string(inst.operands[k]);
                        }
                        out += ", " + std::to_string(inst.immI32);
                        break;
                    case Op::FnNativeIs:
                    case Op::MathFn: {
                        const IntrinsicInfo* info = intrinsicInfo(inst.immI32);
                        out += inst.op == Op::MathFn ? std::string("math.") : std::string("fn.native_is ");
                        if (inst.op == Op::FnNativeIs) {
                            out += "%" + std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) + ", ";
                        }
                        out += info != nullptr ? info->name : "?";
                        for (size_t k = inst.op == Op::FnNativeIs ? 1 : 0; k < inst.operands.size(); ++k) {
                            out += (k == 0 ? " %" : ", %") + std::to_string(inst.operands[k]);
                        }
                        break;
                    }
                    case Op::FnCodeIs:
                        out += "fn.code_is %" +
                               std::to_string(inst.operands.empty() ? 0 : inst.operands[0]) +
                               ", @" + getFnName(inst.calleeIndex);
                        break;
                    case Op::Print:
                    case Op::PrintErr:
                    case Op::KeepAlive:
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
                                out += " @" + getFnName(inst.calleeIndex);
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
                            // The point in the caller's G after this call
                            // (il.h `resumeAfter`), which the inliner reads.
                            if (inst.resumeAfter != Instruction::kNoResumeAfter) {
                                out += " resume.after " + std::to_string(inst.resumeAfter);
                            }
                        } else {
                            for (size_t i = 0; i < inst.operands.size(); ++i) {
                                out += i == 0 ? " " : ", ";
                                out += "%" + std::to_string(inst.operands[i]);
                            }
                            if (inst.op == Op::InBounds && inst.immI32 != 0) {
                                out += ", +" + std::to_string(inst.immI32);
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
