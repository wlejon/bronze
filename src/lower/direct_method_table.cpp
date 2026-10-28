#include "lower/direct_method_table.h"

#include <cstdlib>
#include <unordered_set>
#include <vector>
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
    auto& methods = classMethods_[className];
    const auto existing = methods.find(method);
    if (existing != methods.end() && existing->second != fnIndex) {
        ambiguousClasses_.insert(className);
    }
    methods[method] = fnIndex;
    if (!superName.empty()) {
        const auto existingSuper = classSuper_.find(className);
        if (existingSuper != classSuper_.end() && existingSuper->second != superName) {
            ambiguousClasses_.insert(className);
        }
        classSuper_[className] = superName;
    }
}

void DirectMethodTable::recordClassSuper(const std::string& className,
                                         const std::string& superName) {
    if (className.empty()) return;
    if (!declaredClasses_.insert(className).second) {
        ambiguousClasses_.insert(className);
    }
    if (superName.empty()) return;
    const auto existing = classSuper_.find(className);
    if (existing != classSuper_.end() && existing->second != superName) {
        ambiguousClasses_.insert(className);
    }
    classSuper_[className] = superName;
}

void DirectMethodTable::recordClassAccessor(const std::string& className, const std::string& key,
                                            bool setter, uint32_t fnIndex) {
    if (className.empty() || key.empty()) return;
    AccessorPair& pair = classAccessors_[className][key];
    uint32_t& half = setter ? pair.setter : pair.getter;
    // A second definition of one half under one class name is a second class
    // of that name, or a redefinition: either way the name stops naming one
    // function.
    if (half != il::Instruction::kNoDirectTarget && half != fnIndex) ambiguousClasses_.insert(className);
    half = fnIndex;
}

void DirectMethodTable::recordAccessorSite(uint32_t icIndex, std::string receiverClass, std::string key) {
    if (receiverClass.empty() || key.empty()) return;
    accessorSites_[icIndex] = Site{std::move(receiverClass), std::move(key)};
}

bool DirectMethodTable::isMethodSealedOnSubclasses(uint32_t icIndex) const {
    const auto site = sites_.find(icIndex);
    if (site == sites_.end() || site->second.receiverClass.empty()) return false;
    const std::string& recvClass = site->second.receiverClass;
    const std::string& method = site->second.method;

    std::string cls = recvClass;
    for (size_t hops = 0; hops < 64 && !cls.empty(); ++hops) {
        if (ambiguousClasses_.count(cls) != 0) return false;
        const auto up = classSuper_.find(cls);
        if (up == classSuper_.end()) break;
        cls = up->second;
    }

    for (const auto& [subCls, methods] : classMethods_) {
        if (subCls == recvClass) continue;
        if (methods.find(method) == methods.end()) continue;
        if (ambiguousClasses_.count(subCls) != 0) return false;
        std::string cur = subCls;
        for (size_t hops = 0; hops < 64 && !cur.empty(); ++hops) {
            const auto up = classSuper_.find(cur);
            if (up == classSuper_.end()) break;
            cur = up->second;
            if (cur == recvClass || ambiguousClasses_.count(cur) != 0) return false;
        }
        // The chain ended at a name no class here declares: a base imported
        // from another module under its own binding, or an `extends <expr>`.
        // Either may be the receiver's class, so the override may be its.
        if (cur != subCls && declaredClasses_.count(cur) == 0) return false;
    }
    return true;
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

void DirectMethodTable::resolveAccessors(il::Module& ilModule) const {
    // BRONZE_NO_DIRECT_ACCESSOR=1: every accessor site keeps the cache's
    // dispatch. Compile-time for the reason `resolve` gives.
    static const bool disabled = std::getenv("BRONZE_NO_DIRECT_ACCESSOR") != nullptr;
    if (disabled || accessorSites_.empty() || classAccessors_.empty()) return;

    // The nearest class at or above `className` that names `key` at all: an
    // accessor there is the guess, a method there (which a prototype holds
    // in the same place) means there is none to make. As with methods, the
    // overrides below the class are not consulted; the cell's publish is what
    // decides whether the receiver in hand resolves to the guess.
    auto resolveName = [&](const std::string& className, const std::string& key,
                           bool setter) -> uint32_t {
        std::string cls = className;
        for (size_t hops = 0; hops < 64 && !cls.empty(); ++hops) {
            if (ambiguousClasses_.count(cls) != 0) return il::Instruction::kNoDirectTarget;
            if (const auto it = classAccessors_.find(cls); it != classAccessors_.end()) {
                if (const auto a = it->second.find(key); a != it->second.end()) {
                    return setter ? a->second.setter : a->second.getter;
                }
            }
            if (const auto it = classMethods_.find(cls); it != classMethods_.end()) {
                if (it->second.count(key) != 0) return il::Instruction::kNoDirectTarget;
            }
            const auto up = classSuper_.find(cls);
            if (up == classSuper_.end()) break;
            cls = up->second;
        }
        return il::Instruction::kNoDirectTarget;
    };
    // What the site's hit does with the target: calls its body with the
    // receiver as `this` (and, for a setter, the value as its one argument)
    // and no function object, environment or argument vector — so the body
    // must need none of them, and take its operands boxed as the site holds
    // them.
    auto callable = [&](uint32_t target, bool setter) {
        if (target >= ilModule.functions.size()) return false;
        const il::Function& fn = ilModule.functions[target];
        if (fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody() || fn.isGenerator) return false;
        if (fn.needsEnv || fn.needsArguments || fn.hasRestParam || !fn.needsThis) return false;
        if (!il::functionEntersDirectly(fn)) return false;
        if (fn.callerParamCount() != (setter ? 1u : 0u)) return false;
        for (const auto& p : fn.params) {
            if (p.type != il::Type::Dynamic) return false;
        }
        return fn.returnType == il::Type::Dynamic || fn.returnType == il::Type::Void;
    };

    std::unordered_map<uint32_t, uint32_t> cellOf;
    for (auto& fn : ilModule.functions) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (inst.op != il::Op::PropGet && inst.op != il::Op::PropSet) continue;
                if (inst.staticSlot != il::Instruction::kNoStaticSlot ||
                    inst.directTarget != il::Instruction::kNoDirectTarget) {
                    continue;
                }
                const auto site = accessorSites_.find(inst.icIndex);
                if (site == accessorSites_.end()) continue;
                const bool setter = inst.op == il::Op::PropSet;
                const uint32_t target = resolveName(site->second.receiverClass, site->second.method, setter);
                if (!callable(target, setter)) continue;
                auto cell = cellOf.find(inst.icIndex);
                if (cell == cellOf.end()) {
                    cell = cellOf.emplace(inst.icIndex, ilModule.staticSiteCount).first;
                    ilModule.staticSiteCount += 2;
                }
                inst.directTarget = target;
                inst.staticCellIndex = cell->second;
            }
        }
    }
}

void DirectMethodTable::markOwnCalleeSites(il::Module& ilModule) const {
    if (classMethods_.empty()) return;
    std::unordered_set<std::string> methodNames;
    for (const auto& [cls, methods] : classMethods_) {
        for (const auto& [name, fnIndex] : methods) methodNames.insert(name);
    }
    std::vector<uint8_t> written(ilModule.keyConstants.size(), 0);
    bool any = false;
    for (const auto& fn : ilModule.functions) {
        for (const auto& block : fn.blocks) {
            for (const auto& inst : block.instructions) {
                if (inst.op != il::Op::PropSet || inst.keyIndex >= written.size() || written[inst.keyIndex]) continue;
                if (methodNames.count(ilModule.keyConstants[inst.keyIndex]) == 0) continue;
                written[inst.keyIndex] = 1;
                any = true;
            }
        }
    }
    if (!any) return;
    for (auto& fn : ilModule.functions) {
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (inst.op == il::Op::MethodCall && inst.keyIndex < written.size() && written[inst.keyIndex]) {
                    inst.immI32 |= il::Instruction::kMethodOwnCallee;
                }
            }
        }
    }
}

}  // namespace bronze::lower
