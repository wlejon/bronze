#include "lower/direct_method_table.h"

#include <cstdlib>
#include <string>
#include <utility>

namespace bronze::lower {

void DirectMethodTable::recordSite(uint32_t icIndex, std::string receiverClass,
                                   std::string method) {
    if (method.empty()) return;
    Site site;
    site.receiverClass = std::move(receiverClass);
    site.method = std::move(method);
    sites_[icIndex] = std::move(site);
}

void DirectMethodTable::recordClassMethod(const std::string& className,
                                          const std::string& superName,
                                          const std::string& method, uint32_t fnIndex) {
    if (className.empty() || method.empty()) return;
    classMethods_[className][method] = fnIndex;
    recordClassSuper(className, superName);
}

void DirectMethodTable::recordClassSuper(const std::string& className,
                                         const std::string& superName) {
    if (className.empty() || superName.empty()) return;
    classSuper_[className] = superName;
}

void DirectMethodTable::resolve(il::Module& ilModule) const {
    // BRONZE_NO_DIRECT_METHOD=1. Compile-time, exactly like the static-shape
    // and family-guard seams and for the same reason: what it gates is a SHAPE
    // OF EMITTED CODE, and a runtime toggle would have to be a load and a
    // branch on the fast path it exists to measure.
    static const bool disabled = std::getenv("BRONZE_NO_DIRECT_METHOD") != nullptr;
    if (disabled) return;
    if (sites_.empty() || classMethods_.empty()) return;

    // The nearest declaration of `method` at or above `className` — the up-half
    // of a virtual call's target set. The down-half (overrides in subclasses)
    // is deliberately not consulted: the guess names ONE function and the
    // backend's code-pointer compare is what decides whether the receiver in
    // hand really resolves to it.
    auto resolveName = [&](const std::string& className,
                           const std::string& method) -> uint32_t {
        std::string cls = className;
        // A bounded walk: an `extends` cycle is not expressible in JS, but the
        // table is built from names and a malformed one must not hang lowering.
        for (size_t hops = 0; hops < 64 && !cls.empty(); ++hops) {
            if (const auto it = classMethods_.find(cls); it != classMethods_.end()) {
                if (const auto m = it->second.find(method); m != it->second.end()) {
                    return m->second;
                }
            }
            const auto up = classSuper_.find(cls);
            if (up == classSuper_.end()) break;
            cls = up->second;
        }
        return il::Instruction::kNoDirectTarget;
    };

    for (auto& fn : ilModule.functions) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (inst.op != il::Op::MethodCall) continue;
                const auto site = sites_.find(inst.icIndex);
                if (site == sites_.end() || site->second.receiverClass.empty()) continue;
                const uint32_t target =
                    resolveName(site->second.receiverClass, site->second.method);
                if (target >= ilModule.functions.size()) continue;
                const il::Function& callee = ilModule.functions[target];
                // The shapes a fixed operand list cannot express, exactly as
                // `directCallShapeFits` states them for a named call: an
                // `arguments` object and a rest array are both built from an
                // argument count only the wrapper sees, and no f64 slot can
                // hold the `undefined` a short call would pad with.
                if (callee.needsArguments || callee.hasRestParam) continue;
                const size_t argc = inst.operands.size() - 1;
                const size_t fixed = callee.callerParamCount();
                if (argc > fixed) continue;
                const size_t base = callee.firstSourceParam();
                bool padsTyped = false;
                for (size_t i = argc; i < fixed; ++i) {
                    if (callee.params[i + base].type != il::Type::Dynamic) padsTyped = true;
                }
                if (padsTyped) continue;
                inst.directTarget = target;
            }
        }
    }
}

}  // namespace bronze::lower
