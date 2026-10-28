#include "lower/typed_entry.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>

#include "lower/guard_region_cfg.h"

namespace bronze::lower {

namespace {

// How many signatures one method may have entries for. Each is a copy of the
// whole body, so the bound is on build time; three.js's hot methods are called
// with one signature, occasionally two.
constexpr size_t kMaxEntriesPerMethod = 3;

bool envFlag(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

std::vector<const il::Instruction*> definitionsOf(const il::Function& fn) {
    std::vector<const il::Instruction*> defs(fn.valueCount, nullptr);
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < defs.size()) defs[inst.result] = &inst;
        }
    }
    return defs;
}

char signatureLetter(il::Type t) {
    switch (t) {
        case il::Type::F64: return 'f';
        case il::Type::Bool: return 'b';
        default: return 'd';
    }
}

using EntryKey = std::pair<uint32_t, std::string>;

// A signature's spelling: a letter per parameter, `u` for one the edge omits.
std::string keyOf(const TypedSignature& sig) {
    std::string k;
    for (size_t a = 0; a < sig.params.size(); ++a) k += a >= sig.argc ? 'u' : signatureLetter(sig.params[a]);
    return k;
}

// The name a stack frame shows for the generic function, so a frame of its
// typed entry reads the same (brass_backend_sections.cpp names a frame by
// `displayName`, falling back to the IL name).
std::string frameNameOf(const il::Function& fn) {
    if (!fn.displayName.empty()) return fn.displayName;
    if (fn.name.rfind("__anon_fn", 0) == 0) return "<anonymous>";
    return fn.name;
}

template <typename Fn>
void forEachUseSlot(il::Instruction& inst, Fn&& fn) {
    for (il::ValueId& v : inst.operands) fn(v);
    for (il::ValueId& v : inst.target.args) fn(v);
    for (il::ValueId& v : inst.elseTarget.args) fn(v);
}

// The copy: `origin`'s finished body, each typed parameter re-boxed once at
// the entry and every read of the parameter reading that box, and every read
// of an omitted one reading `undefined`. Nothing else changes, so for the
// arguments of the signature the copy is the origin.
il::Function makeEntry(const il::Function& origin, uint32_t originIndex, const TypedSignature& sig) {
    il::Function entry = origin;
    entry.name = origin.name + "$typed_" + keyOf(sig);
    entry.displayName = frameNameOf(origin);
    entry.isExported = false;
    entry.typedOrigin = originIndex;
    entry.typedArgc = static_cast<uint32_t>(sig.argc);

    const size_t first = entry.firstSourceParam();
    std::vector<il::ValueId> boxOf(entry.valueCount, il::kNoValue);
    std::vector<il::Instruction> boxes;
    for (size_t a = 0; a < sig.params.size(); ++a) {
        const il::ValueId param = static_cast<il::ValueId>(first + a);
        if (a >= sig.argc) {
            il::Instruction undef;
            undef.op = il::Op::ConstUndefined;
            undef.type = il::Type::Dynamic;
            undef.result = entry.valueCount++;
            boxOf[param] = undef.result;
            boxes.push_back(std::move(undef));
            continue;
        }
        if (sig.params[a] == il::Type::Dynamic) continue;
        entry.params[param].type = sig.params[a];
        il::Instruction box;
        box.op = il::Op::Box;
        box.type = il::Type::Dynamic;
        box.boxType = sig.params[a];
        box.result = entry.valueCount++;
        box.operands = {param};
        boxOf[param] = box.result;
        boxes.push_back(std::move(box));
    }
    for (auto& block : entry.blocks) {
        for (auto& inst : block.instructions) {
            forEachUseSlot(inst, [&](il::ValueId& v) {
                if (v < boxOf.size() && boxOf[v] != il::kNoValue) v = boxOf[v];
            });
        }
    }
    auto& head = entry.blocks.front().instructions;
    head.insert(head.begin(), boxes.begin(), boxes.end());
    return entry;
}

// `box.<t> %x` arguments whose box no instruction reads any more: the edge now
// passes `%x` itself.
void dropDeadArgumentBoxes(il::Function& fn) {
    std::vector<uint8_t> used(fn.valueCount, 0);
    for (auto& block : fn.blocks) {
        for (auto& inst : block.instructions) {
            forEachUseSlot(inst, [&](il::ValueId& v) {
                if (v < used.size()) used[v] = 1;
            });
        }
    }
    for (auto& block : fn.blocks) {
        auto& insts = block.instructions;
        std::erase_if(insts, [&](const il::Instruction& inst) {
            return inst.op == il::Op::Box &&
                   (inst.boxType == il::Type::F64 || inst.boxType == il::Type::Bool) &&
                   inst.result != il::kNoValue && inst.result < used.size() && !used[inst.result];
        });
    }
}


}  // namespace

bool typedEntryEligible(const il::Function& fn) {
    if (fn.blocks.empty() || fn.isExternal || fn.isEntryPoint) return false;
    if (!fn.needsThis || fn.needsEnv || fn.needsArguments || fn.hasRestParam) return false;
    if (fn.isGenerator || fn.isCoroutineBody()) return false;
    if (fn.typedOrigin != il::Function::kNoTypedOrigin) return false;
    for (size_t p = fn.firstSourceParam(); p < fn.params.size(); ++p) {
        if (fn.params[p].type != il::Type::Dynamic || fn.params[p].pinned) return false;
    }
    return true;
}

bool typedSignatureAt(const il::Module& module, const std::vector<const il::Instruction*>& defs,
                      const il::Instruction& call, TypedSignature& out) {
    if (call.op != il::Op::MethodCall || call.directTarget == il::Instruction::kNoDirectTarget) return false;
    if (call.directTarget >= module.functions.size() || call.operands.empty()) return false;
    const il::Function& target = module.functions[call.directTarget];
    if (!typedEntryEligible(target)) return false;
    // Every parameter passed: the backend's direct edge fills a missing one
    // with `undefined` only where it can, and an entry's typed parameter
    // cannot hold it.
    const size_t argc = call.operands.size() - 1;
    const size_t count = target.callerParamCount();
    if (argc > count) return false;
    out.params.assign(count, il::Type::Dynamic);
    out.unboxed.assign(count, il::kNoValue);
    out.argc = argc;
    bool any = argc < count;
    for (size_t a = 0; a < argc; ++a) {
        const il::ValueId v = call.operands[a + 1];
        const il::Instruction* def = v < defs.size() ? defs[v] : nullptr;
        if (def == nullptr || def->op != il::Op::Box || def->operands.size() != 1) continue;
        if (def->boxType != il::Type::F64 && def->boxType != il::Type::Bool) continue;
        out.params[a] = def->boxType;
        out.unboxed[a] = def->operands[0];
        any = true;
    }
    return any;
}

void applyTypedEntries(il::Module& module, TypedEntryStats* stats) {
    static const bool disabled = envFlag("BRONZE_NO_TYPED_ENTRIES");
    if (disabled) return;
    TypedEntryStats local;
    TypedEntryStats& st = stats != nullptr ? *stats : local;

    // Built entries by (generic function, signature); `kNoDirectTarget` for a
    // signature that was tried and not kept, so it is not tried again.
    std::map<EntryKey, uint32_t> entries;
    std::map<uint32_t, size_t> entriesPerMethod;

    // Rounds: an entry is a copy of a body that may itself call methods with
    // unboxed arguments, and those signatures are only seen once it exists.
    size_t scanFrom = 0;
    while (scanFrom < module.functions.size()) {
        const size_t scanTo = module.functions.size();
        std::vector<std::pair<uint32_t, TypedSignature>> wanted;
        for (size_t f = scanFrom; f < scanTo; ++f) {
            const il::Function& fn = module.functions[f];
            if (fn.blocks.empty()) continue;
            const auto defs = definitionsOf(fn);
            for (const auto& block : fn.blocks) {
                for (const auto& inst : block.instructions) {
                    TypedSignature sig;
                    if (!typedSignatureAt(module, defs, inst, sig)) continue;
                    EntryKey key{inst.directTarget, keyOf(sig)};
                    if (entries.count(key) != 0) continue;
                    entries.emplace(key, il::Instruction::kNoDirectTarget);
                    wanted.emplace_back(inst.directTarget, std::move(sig));
                }
            }
        }
        scanFrom = scanTo;
        for (auto& [origin, sig] : wanted) {
            if (entriesPerMethod[origin] >= kMaxEntriesPerMethod) {
                ++st.entriesCapped;
                continue;
            }
            il::Function entry = makeEntry(module.functions[origin], origin, sig);
            const uint32_t folds = foldTypedEntry(entry);
            if (folds == 0) {
                ++st.entriesNoGain;
                continue;
            }
            if (!dominanceHolds(entry)) {
                ++st.entriesRefused;
                continue;
            }
            st.foldsApplied += folds;
            ++st.entriesBuilt;
            ++entriesPerMethod[origin];
            entries[EntryKey{origin, keyOf(sig)}] = static_cast<uint32_t>(module.functions.size());
            module.functions.push_back(std::move(entry));
        }
    }

    // The rewrite reads the same predicate the entry set was built from, and
    // rewires a site only to an entry for exactly its signature.
    for (auto& fn : module.functions) {
        if (fn.blocks.empty()) continue;
        const auto defs = definitionsOf(fn);
        bool rewired = false;
        for (auto& block : fn.blocks) {
            for (auto& inst : block.instructions) {
                if (inst.op == il::Op::MethodCall && inst.directTarget != il::Instruction::kNoDirectTarget) {
                    ++st.sitesDirect;
                }
                TypedSignature sig;
                if (!typedSignatureAt(module, defs, inst, sig)) continue;
                ++st.sitesUnboxed;
                const auto it = entries.find(EntryKey{inst.directTarget, keyOf(sig)});
                if (it == entries.end() || it->second == il::Instruction::kNoDirectTarget) continue;
                for (size_t a = 0; a < sig.params.size(); ++a) {
                    if (sig.params[a] != il::Type::Dynamic) inst.operands[a + 1] = sig.unboxed[a];
                }
                inst.typedTarget = it->second;
                ++st.sitesTyped;
                rewired = true;
            }
        }
        if (rewired) dropDeadArgumentBoxes(fn);
    }

    if (envFlag("BRONZE_TYPED_ENTRY_STATS")) {
        std::fprintf(stderr,
                     "typed entries: %u direct sites, %u with unboxed arguments, %u typed; "
                     "%u entries built (%u folds), %u no gain, %u refused, %u capped\n",
                     st.sitesDirect, st.sitesUnboxed, st.sitesTyped, st.entriesBuilt, st.foldsApplied,
                     st.entriesNoGain, st.entriesRefused, st.entriesCapped);
    }
}

}  // namespace bronze::lower
