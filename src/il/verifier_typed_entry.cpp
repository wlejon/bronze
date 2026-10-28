#include "il/verifier_proofs.h"

#include <string>
#include <vector>

namespace bronze::il {

// TYPED ENTRIES (lower/typed_entry.h). Two facts make a typed edge sound, and
// both are checked here rather than trusted from the pass that built it:
//
//   - the CALLEE is a typed entry of the site's own `directTarget`, so the
//     identity the backend's guard compares (the generic function's wrapper)
//     names the function whose body the typed entry is a copy of;
//   - the edge passes exactly the arguments the entry was built for: past
//     them its body reads `undefined`, which is what the backend fills in;
//   - every operand in a typed position IS the type the entry's parameter
//     takes. The backend hands the operand over unconverted, so an operand of
//     another type would be a raw bitcast of a boxed value — the one way this
//     edge could be wrong, and exactly the fact a whole-program join cannot be
//     relied on for. Here it is a fact about the caller's own SSA value.
//
// And of the entry itself: it is a copy of an ordinary method (not an env
// closure, no `arguments`, no rest parameter, not a coroutine), and only the
// types of its source parameters differ from the function it is a copy of.
bool verifyTypedEntries(const Module& module, DiagnosticSink& diags) {
    const size_t n = module.functions.size();
    auto fail = [&](const std::string& fnName, const std::string& msg) {
        diags.error(Span{}, "Function " + fnName + ": " + msg);
        return false;
    };
    for (const Function& fn : module.functions) {
        if (fn.typedOrigin == Function::kNoTypedOrigin) continue;
        if (fn.typedOrigin >= n) return fail(fn.name, "typed entry of a function past the module");
        const Function& origin = module.functions[fn.typedOrigin];
        if (origin.typedOrigin != Function::kNoTypedOrigin) {
            return fail(fn.name, "typed entry of a typed entry");
        }
        if (!fn.needsThis || fn.needsEnv || fn.needsArguments || fn.hasRestParam ||
            fn.isCoroutineBody() || fn.isGenerator || fn.isExternal || fn.isEntryPoint) {
            return fail(fn.name, "a typed entry is a plain method body");
        }
        if (fn.params.size() != origin.params.size() || fn.needsThis != origin.needsThis ||
            fn.firstSourceParam() != origin.firstSourceParam()) {
            return fail(fn.name, "typed entry's parameter list differs from " + origin.name + "'s");
        }
        if (fn.typedArgc > fn.callerParamCount()) {
            return fail(fn.name, "typed entry's edge arity is past its parameter list");
        }
        for (size_t p = 0; p < fn.params.size(); ++p) {
            const Type t = fn.params[p].type;
            if (p >= fn.firstSourceParam() + fn.typedArgc && t != Type::Dynamic) {
                return fail(fn.name, "typed entry types a parameter its edges omit");
            }
            const Type o = origin.params[p].type;
            if (t == o) continue;
            if (p < fn.firstSourceParam() || o != Type::Dynamic ||
                (t != Type::F64 && t != Type::Bool)) {
                return fail(fn.name, "typed entry parameter " + std::to_string(p) +
                                         " is neither its origin's type nor an unboxed f64/bool");
            }
        }
    }

    for (const Function& fn : module.functions) {
        // Value types, by id: what every operand is defined as.
        std::vector<Type> types(fn.valueCount, Type::Void);
        for (size_t p = 0; p < fn.params.size() && p < types.size(); ++p) types[p] = fn.params[p].type;
        for (const Block& block : fn.blocks) {
            for (const BlockParam& bp : block.params) {
                if (bp.id < types.size()) types[bp.id] = bp.type;
            }
            for (const Instruction& inst : block.instructions) {
                if (inst.result != kNoValue && inst.result < types.size()) types[inst.result] = inst.type;
            }
        }
        for (const Block& block : fn.blocks) {
            for (const Instruction& inst : block.instructions) {
                if (inst.typedTarget == Instruction::kNoDirectTarget) continue;
                if (inst.op != Op::MethodCall) return fail(fn.name, "a typed edge on a non-method call");
                if (inst.typedTarget >= n || inst.directTarget >= n) {
                    return fail(fn.name, "typed edge names a function past the module");
                }
                const Function& entry = module.functions[inst.typedTarget];
                if (entry.typedOrigin != inst.directTarget) {
                    return fail(fn.name, "typed edge @" + std::to_string(inst.typedTarget) +
                                             " is not a typed entry of its direct target @" +
                                             std::to_string(inst.directTarget));
                }
                const size_t argc = inst.operands.empty() ? 0 : inst.operands.size() - 1;
                if (argc != entry.typedArgc) {
                    return fail(fn.name, "typed edge passes " + std::to_string(argc) + " arguments where " +
                                             entry.name + "'s edges pass " + std::to_string(entry.typedArgc));
                }
                for (size_t a = 0; a < entry.callerParamCount(); ++a) {
                    const Type want = entry.params[entry.firstSourceParam() + a].type;
                    if (a >= argc) {
                        if (want != Type::Dynamic) {
                            return fail(fn.name, "typed edge omits typed argument " + std::to_string(a));
                        }
                        continue;
                    }
                    const ValueId v = inst.operands[a + 1];
                    const Type have = v < types.size() ? types[v] : Type::Void;
                    if (have != want) {
                        return fail(fn.name, "typed edge argument " + std::to_string(a) + " is " +
                                                 typeName(have) + " where " + entry.name + " takes " +
                                                 typeName(want));
                    }
                }
            }
        }
    }
    return true;
}

}  // namespace bronze::il
