// The closure guesses (direct_method_table.h, `closureGuesses`): which
// functions' closures a method-call site's receiver may hold in an own slot
// under the site's key, read off the program's own stores of that key.
//
// An AOT build has no profile to say which closure a callback slot holds, and
// every receiver holds a different closure object (Object3D's constructor
// makes a fresh `onRotationChange` for each Euler it owns), so a guard on the
// closure's identity would never hit. What the program does fix is the CODE:
// the stores of the key are few, and each stores either a function expression
// made right there or a parameter that every caller fills with one. The
// guesses are those functions; the inliner copies their bodies behind a test
// of the slot's closure's code word and hands them that closure's environment.
// A wrong or missing guess costs the test and nothing else.
//
// Only one level of parameter flow is followed (`_onChange(cb) { this.k = cb;
// }` called with `function f() {}`); a value from anywhere else is not a
// guess, and a store is counted for a site only when their receivers' classes
// are related (Quaternion's `_onChange` is not, for an Euler site).

#include "lower/direct_method_table.h"

#include <algorithm>
#include <unordered_set>

namespace bronze::lower {

namespace {

// The defining instruction of every value of `fn` (parameters have none).
std::vector<const il::Instruction*> defsOf(const il::Function& fn) {
    std::vector<const il::Instruction*> d(fn.valueCount, nullptr);
    for (const auto& b : fn.blocks) {
        for (const auto& inst : b.instructions) {
            if (inst.result != il::kNoValue && inst.result < d.size()) d[inst.result] = &inst;
        }
    }
    return d;
}

void addUnique(std::vector<uint32_t>& out, uint32_t f) {
    if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
}

}  // namespace

bool DirectMethodTable::classesRelated(const std::string& a, const std::string& b) const {
    if (a.empty() || b.empty() || a == b) return true;
    const auto below = [&](const std::string& lo, const std::string& hi) {
        std::string c = lo;
        for (size_t hops = 0; hops < 64; ++hops) {
            const auto up = classSuper_.find(c);
            if (up == classSuper_.end()) return false;
            c = up->second;
            if (c == hi) return true;
        }
        return false;
    };
    return below(a, b) || below(b, a);
}

std::unordered_map<uint32_t, std::vector<uint32_t>> DirectMethodTable::closureGuesses(
    const il::Module& ilModule) const {
    std::unordered_map<uint32_t, std::vector<uint32_t>> out;
    const size_t numFns = ilModule.functions.size();
    std::vector<std::vector<const il::Instruction*>> defs(numFns);
    for (size_t f = 0; f < numFns; ++f) defs[f] = defsOf(ilModule.functions[f]);

    // The function a value of `f` is a fresh closure of, if it is one.
    const auto closureOf = [&](size_t f, il::ValueId v) -> uint32_t {
        const il::Instruction* d = v < defs[f].size() ? defs[f][v] : nullptr;
        return d && d->op == il::Op::CreateFunction ? d->calleeIndex : il::Instruction::kNoDirectTarget;
    };

    // Every resolved method call into a function, by that function.
    std::unordered_map<uint32_t, std::vector<std::pair<size_t, const il::Instruction*>>> callsTo;
    for (size_t f = 0; f < numFns; ++f) {
        for (const auto& b : ilModule.functions[f].blocks) {
            for (const auto& inst : b.instructions) {
                if (inst.op == il::Op::MethodCall && inst.directTarget != il::Instruction::kNoDirectTarget) {
                    callsTo[inst.directTarget].emplace_back(f, &inst);
                }
            }
        }
    }

    // Per key: (class of the store's receiver, the functions it stores).
    struct Store {
        std::string cls;
        std::vector<uint32_t> fns;
    };
    std::unordered_map<uint32_t, std::vector<Store>> stores;
    for (size_t f = 0; f < numFns; ++f) {
        const il::Function& fn = ilModule.functions[f];
        const size_t firstSource = fn.firstSourceParam();
        for (const auto& b : fn.blocks) {
            for (const auto& inst : b.instructions) {
                if (inst.op != il::Op::PropSet || inst.operands.size() < 2) continue;
                const il::ValueId v = inst.operands[1];
                Store st;
                const auto cls = accessorSites_.find(inst.icIndex);
                if (cls != accessorSites_.end() && inst.keyIndex < ilModule.keyConstants.size() &&
                    cls->second.method == ilModule.keyConstants[inst.keyIndex]) {
                    st.cls = cls->second.receiverClass;
                }
                const uint32_t direct = closureOf(f, v);
                if (direct != il::Instruction::kNoDirectTarget) {
                    addUnique(st.fns, direct);
                } else if (v < fn.params.size() && v >= firstSource) {
                    // A parameter: what each resolved call into `fn` passes.
                    const size_t arg = 1 + (v - firstSource);
                    const auto calls = callsTo.find(static_cast<uint32_t>(f));
                    if (calls == callsTo.end()) continue;
                    for (const auto& [caller, call] : calls->second) {
                        if (arg >= call->operands.size()) continue;
                        const uint32_t g = closureOf(caller, call->operands[arg]);
                        if (g != il::Instruction::kNoDirectTarget) addUnique(st.fns, g);
                    }
                }
                if (!st.fns.empty()) stores[inst.keyIndex].push_back(std::move(st));
            }
        }
    }
    if (stores.empty()) return out;

    // A store and a site are matched by class: the same class or one above
    // the other. Where neither receiver's class is known (an object literal's
    // methods, `ColorManagement.convert`), only for a key no class declares a
    // method of: a key such as `set` or `add` names a method of half the
    // classes of a library, and its unknown receivers are mostly those.
    std::unordered_set<uint32_t> classMethodKeys;
    {
        std::unordered_set<std::string> names;
        for (const auto& [cls, methods] : classMethods_) {
            for (const auto& [name, fnIndex] : methods) names.insert(name);
        }
        for (uint32_t k = 0; k < ilModule.keyConstants.size(); ++k) {
            if (names.count(ilModule.keyConstants[k])) classMethodKeys.insert(k);
        }
    }

    for (const auto& fn : ilModule.functions) {
        for (const auto& b : fn.blocks) {
            for (const auto& inst : b.instructions) {
                if (inst.op != il::Op::MethodCall || out.count(inst.icIndex)) continue;
                const auto st = stores.find(inst.keyIndex);
                if (st == stores.end()) continue;
                const auto site = sites_.find(inst.icIndex);
                const std::string cls = site == sites_.end() ? std::string() : site->second.receiverClass;
                std::vector<uint32_t> guesses;
                for (const Store& s : st->second) {
                    if (cls.empty() != s.cls.empty()) continue;
                    if (cls.empty() ? classMethodKeys.count(inst.keyIndex) != 0 : !classesRelated(cls, s.cls)) {
                        continue;
                    }
                    for (uint32_t g : s.fns) addUnique(guesses, g);
                }
                if (!guesses.empty() && guesses.size() <= kMaxClosureGuesses) out[inst.icIndex] = std::move(guesses);
            }
        }
    }
    return out;
}

}  // namespace bronze::lower
