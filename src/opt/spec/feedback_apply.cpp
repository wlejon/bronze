// The consuming half of type-feedback specialization (feedback_plan.h): the
// accessor targets (F) before guarded inlining, and the static-slot claims (A)
// and number hints (C) on the finished module. The direct targets (B) are
// feedback_methods.cpp's.

#include <algorithm>
#include <charconv>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "abi/bronze_abi.h"
#include "il/feedback_site.h"
#include "il/key.h"
#include "opt/spec/feedback_plan.h"
#include "opt/spec/feedback_util.h"
#include "opt/wpo/stamp.h"

namespace bronze::spec {

namespace {

constexpr uint32_t kNone = UINT32_MAX;

}  // namespace

// A shape name `<prototype>{k1,k2,...}` (runtime/type_feedback.cpp) split into
// its keys, in slot order. False for a dictionary, a receiver named by kind
// (`array`, `number`, ...) or an unparsable name. `complete` is false when the
// recorder cut the list short (`+N`).
bool shapeKeys(const std::string& name, std::vector<std::string_view>& keys, bool& complete,
               uint32_t* inlineSlots) {
    const size_t open = name.find('{');
    if (open == std::string::npos || name.empty() || name.back() != '}') return false;
    if (inlineSlots != nullptr) {
        // `#N` before the brace: an inline capacity other than the default.
        *inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
        const size_t hash = name.rfind('#', open);
        if (hash != std::string::npos) {
            uint32_t n = 0;
            const auto [end, ec] = std::from_chars(name.data() + hash + 1, name.data() + open, n);
            if (ec != std::errc() || end != name.data() + open || n < BRONZE_ABI_OBJ_INLINE_SLOTS ||
                n > BRONZE_ABI_OBJ_OVERFLOW_SLOT) {
                return false;
            }
            *inlineSlots = n;
        }
    }
    const std::string_view body = std::string_view(name).substr(open + 1, name.size() - open - 2);
    if (body == "dict" || body == "?") return false;
    keys.clear();
    complete = true;
    if (body.empty()) return true;
    size_t at = 0;
    while (at <= body.size()) {
        const size_t comma = std::min(body.find(',', at), body.size());
        const std::string_view key = body.substr(at, comma - at);
        if (comma == body.size() && !key.empty() && key.front() == '+') {
            complete = false;
            break;
        }
        keys.push_back(key);
        at = comma + 1;
    }
    return true;
}

namespace {

// The instructions strictly inside a transition run: nothing may be claimed
// there, the run's own guard answers them (lower/transition_runs.h).
std::vector<uint8_t> runInteriors(const il::Block& block) {
    std::vector<uint8_t> interior(block.instructions.size(), 0);
    for (size_t i = 0; i < block.instructions.size(); ++i) {
        const il::Instruction& inst = block.instructions[i];
        if (inst.transitionRunCell == il::Instruction::kNoTransitionRun) continue;
        for (size_t j = i; j < block.instructions.size() && j < i + inst.transitionRunLength; ++j) interior[j] = 1;
    }
    return interior;
}

class ClaimWriter {
public:
    ClaimWriter(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats& stats)
        : mod_(mod), plan_(plan), stats_(stats) {
        for (uint32_t k = 0; k < mod.keyConstants.size(); ++k) keyIndex_.emplace(mod.keyConstants[k], k);
    }

    // A: one property site.
    // A read whose receiver names a class (il.h `icFnRecv`) is not claimed:
    // its receiver is a function, whose statics box is the shape the harvest
    // names, and a claim's guard passes plain objects only.
    void claim(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.staticSlot != il::Instruction::kNoStaticSlot || inst.directTarget != il::Instruction::kNoDirectTarget ||
            inst.icFnRecv || il::isDefineWrite(inst) || inst.operands.empty() ||
            inst.operands[0] >= observable.size() || observable[inst.operands[0]] == 0 ||
            inst.keyIndex >= mod_.keyConstants.size() || il::parseIndexKey(mod_.keyConstants[inst.keyIndex])) {
            return;
        }
        const auto it = plan_.shapesByIc.find(inst.icIndex);
        if (it == plan_.shapesByIc.end() || plan_.ownAccessorIcs.count(inst.icIndex) != 0) return;
        const std::string* shape = it->second.dominant(kDominantPercent);
        if (shape == nullptr) {
            claimPolyFamily(inst, it->second);
            return;
        }
        std::vector<std::string_view> keys;
        bool complete = false;
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        uint32_t slot = kNone;
        uint32_t index = kNone;
        uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
        bool prototypeLike = false;
        if (shapeKeys(*shape, keys, complete, &inlineSlots)) {
            for (uint32_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) index = index == kNone ? i : kNone - 1;
                if (keys[i] == "constructor") prototypeLike = true;
            }
        }
        // The key's position is its slot while inline; past the shape's
        // capacity the slots number from the overflow split.
        slot = index;
        if (index < kNone - 1 && index >= inlineSlots) slot = BRONZE_ABI_OBJ_OVERFLOW_SLOT + (index - inlineSlots);
        const bool write = inst.op == il::Op::PropSet;
        // A write to an object others inherit from must reach the runtime,
        // which moves the prototype epoch (the runtime also refuses to publish
        // a prototype's shape for a store; `constructor` is how a prototype
        // looks from here).
        if (slot >= kNone - 1 || (write && prototypeLike)) {
            ++stats_.shapeRefused;
            return;
        }
        inst.staticSlot = slot;
        inst.staticSlotGuessed = true;
        if (claimedIcs_.insert(inst.icIndex).second) stats_.shapeExecutions += it->second.total;
        uint32_t lo = 0, span = 0;
        // A family claims position i at slot i, which a shape whose capacity
        // splits its keys earlier never has (class_family.cpp). For a write,
        // every family in the range must also declare the slot writable.
        if (complete && slot == index && familyOf(keys, inlineSlots, slot, inst.keyIndex, write, &lo, &span)) {
            inst.staticCellIndex = 0;
            inst.familyLo = lo;
            inst.familySpan = span;
            ++stats_.shapeFamily;
            return;
        }
        inst.staticCellIndex = mod_.staticSiteCount++;
        inst.familyLo = il::Instruction::kNoFamily;
        inst.familySpan = 0;
        // The run also met a shape the claimed one grew from, by one or two
        // keys: an object read here before it was finished, whose shape the
        // cell can latch first (il.h staticRegrow).
        if (complete) {
            for (const auto& [name, count] : it->second.counts) {
                (void)count;
                if (&name == shape) continue;
                std::vector<std::string_view> before;
                bool whole = false;
                if (!shapeKeys(name, before, whole) || !whole || before.size() >= keys.size() ||
                    before.size() + 2 < keys.size() ||
                    std::string_view(name).substr(0, name.find('{')) !=
                        std::string_view(*shape).substr(0, shape->find('{'))) {
                    continue;
                }
                if (std::equal(before.begin(), before.end(), keys.begin())) {
                    inst.staticRegrow = true;
                    ++stats_.shapeRegrow;
                    break;
                }
            }
        }
        ++stats_.shapeIdentity;
    }

    // C: a truthiness unbox whose every observed value was a Boolean, an
    // object, undefined or null: the kinds it saw (the Number case is
    // `hint`'s).
    void truth(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.op != il::Op::Unbox || inst.type != il::Type::Bool || inst.numberHint || inst.truthHint != 0 ||
            inst.operands.empty()) {
            return;
        }
        const il::ValueId v = inst.operands[0];
        if (v >= observable.size() || observable[v] == 0) return;
        const auto it = plan_.values.find(valueKey(inst, 0));
        if (it == plan_.values.end() || it->second.total < kMinExecutions) return;
        const NumberCounts& c = it->second;
        if (c.bools + c.objects + c.undefineds + c.nulls != c.total) return;
        uint8_t kinds = 0;
        if (c.bools) kinds |= il::Instruction::kTruthBool;
        if (c.objects) kinds |= il::Instruction::kTruthObject;
        if (c.undefineds) kinds |= il::Instruction::kTruthUndefined;
        if (c.nulls) kinds |= il::Instruction::kTruthNull;
        inst.truthHint = kinds;
        ++stats_.truthHints;
        if (hinted_.insert(it->first).second) stats_.hintExecutions += c.total;
    }

    // E: a read's inline way chain (il.h `icChain`). Every read the run met
    // with one to four shapes, each at least 1% of its executions, and
    // nothing else: a site of more shapes is the stub's (its table probe),
    // and one a static claim answers needs no chain for its one shape. The
    // prototype arm goes where the run read a value that is not `undefined`
    // through a shape whose complete key list lacks the key: a data property
    // up the chain (WebGL's constants, on the context's prototype).
    void chain(il::Instruction& inst) {
        if (inst.op != il::Op::PropGet || inst.icChain != 0 || inst.operands.empty() ||
            inst.keyIndex >= mod_.keyConstants.size()) {
            return;
        }
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        if (key == "length" || il::parseIndexKey(key)) return;
        const auto it = plan_.shapesByIc.find(inst.icIndex);
        if (it == plan_.shapesByIc.end() || it->second.total < kMinExecutions) return;
        const NameCounts& names = it->second;
        // The shapes past the 1% line count toward the ways tested, not
        // toward the limit: a rare shape installed late moves the common
        // ones down a way each. A receiver that is no plain object, or a
        // dictionary, holds no way and goes to the stub.
        uint32_t significant = 0, plain = 0;
        bool lacking = false;
        bool allInFamily =
            inst.staticSlot != il::Instruction::kNoStaticSlot && inst.familyLo != il::Instruction::kNoFamily;
        uint64_t ownReads = 0, lackingReads = 0;
        std::vector<std::string_view> keys;
        for (const auto& [name, count] : names.counts) {
            const bool major = count * 100 >= names.total;
            bool complete = false;
            if (name == "*other*") {
                if (major) return;
                if (count != 0) allInFamily = false;
                continue;
            }
            uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
            if (!shapeKeys(name, keys, complete, &inlineSlots)) {
                if (count != 0) allInFamily = false;
                continue;
            }
            if (allInFamily && count != 0) {
                uint32_t shapeLo = 0, shapeSpan = 0;
                if (!complete ||
                    !familyOf(keys, inlineSlots, inst.staticSlot, inst.keyIndex, /*write=*/false, &shapeLo,
                              &shapeSpan) ||
                    shapeLo < inst.familyLo || shapeLo + shapeSpan > inst.familyLo + inst.familySpan) {
                    allInFamily = false;
                }
            }
            ++plain;
            if (!major) continue;
            if (++significant > 4) return;
            bool has = false;
            for (std::string_view k : keys) has = has || k == key;
            if (complete && !has) lacking = true;
            (complete && !has ? lackingReads : ownReads) += count;
        }
        if (significant == 0 || allInFamily || (plain == 1 && inst.staticSlot != il::Instruction::kNoStaticSlot)) {
            return;
        }
        uint8_t chain = static_cast<uint8_t>(std::min<uint32_t>(plain, 4));
        const auto rt = plan_.readResultsByIc.find(inst.icIndex);
        if (lacking && rt != plan_.readResultsByIc.end() && rt->second.undefineds < rt->second.total) {
            chain |= il::Instruction::kChainProto;
            ++stats_.chainProto;
            if (rt->second.undefineds * 2 < rt->second.total) chain |= il::Instruction::kChainProtoFirst;
        }
        // Where most reads lack the key, the answer past the own slots is the
        // one laid out in line (a three.js `object.isInstancedMesh` test).
        if (lackingReads > ownReads) chain |= il::Instruction::kChainNotOwnFirst;
        inst.icChain = chain;
        ++stats_.chainReads;
        if (chainedIcs_.insert(inst.icIndex).second) stats_.chainExecutions += names.total;
    }

    // C: one numeric op.
    void hint(il::Instruction& inst, const std::vector<uint8_t>& observable) {
        if (inst.numberHint || !il::observesOperandTags(inst)) return;
        bool any = false;
        uint64_t executions = 0;
        for (uint32_t j = 0; j < inst.operands.size(); ++j) {
            const il::ValueId v = inst.operands[j];
            if (v >= observable.size() || observable[v] == 0) continue;
            const auto it = plan_.values.find(valueKey(inst, j));
            if (it == plan_.values.end() || it->second.total < kMinExecutions ||
                it->second.numbers != it->second.total) {
                return;
            }
            any = true;
            if (hinted_.insert(it->first).second) executions += it->second.total;
        }
        if (!any) return;
        inst.numberHint = true;
        ++stats_.numberHints;
        stats_.hintExecutions += executions;
    }

    // Whether the run saw a receiver at this claimed site that its claim does
    // not answer (feedback_plan.h, `missedClaims`).
    bool missed(const il::Instruction& inst) const {
        if (inst.staticSlot == il::Instruction::kNoStaticSlot || inst.keyIndex >= mod_.keyConstants.size()) {
            return false;
        }
        const auto it = plan_.shapesByIc.find(inst.icIndex);
        if (it == plan_.shapesByIc.end()) return false;
        const bool family = inst.familyLo != il::Instruction::kNoFamily;
        if (!family && plan_.shapeVariantIcs.count(inst.icIndex) != 0) return true;
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        const bool write = inst.op == il::Op::PropSet;
        uint32_t seen = 0;
        std::vector<std::string_view> keys;
        for (const auto& [name, count] : it->second.counts) {
            if (count == 0) continue;
            bool complete = false;
            uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
            if (name == "*other*" || !shapeKeys(name, keys, complete, &inlineSlots) || !complete) return true;
            if (!family && ++seen > 1) return true;
            uint32_t index = kNone;
            for (uint32_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) index = index == kNone ? i : kNone - 1;
            }
            if (index >= kNone - 1) return true;
            const uint32_t slot =
                index >= inlineSlots ? BRONZE_ABI_OBJ_OVERFLOW_SLOT + (index - inlineSlots) : index;
            if (slot != inst.staticSlot) return true;
            if (!family) continue;
            uint32_t lo = 0, span = 0;
            if (!familyOf(keys, inlineSlots, slot, inst.keyIndex, write, &lo, &span) || lo < inst.familyLo ||
                lo + span > inst.familyLo + inst.familySpan) {
                return true;
            }
        }
        return false;
    }

private:
    // A polymorphic site whose major shapes belong to one class-family range
    // that agrees on the slot (and writability, for a store): one range test
    // covers every subclass in `[lo, hi]` without pinning a single shape.
    void claimPolyFamily(il::Instruction& inst, const NameCounts& names) {
        if (names.total < kMinExecutions || mod_.classFamilies.empty()) return;
        const bool write = inst.op == il::Op::PropSet;
        const std::string& key = mod_.keyConstants[inst.keyIndex];
        uint32_t commonSlot = kNone;
        uint32_t lo = kNone, hi = 0;
        uint64_t covered = 0;
        std::vector<std::string_view> keys;
        for (const auto& [name, count] : names.counts) {
            if (count == 0 || count * 100 < names.total) continue;
            if (name == "*other*") return;
            bool complete = false;
            uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
            if (!shapeKeys(name, keys, complete, &inlineSlots) || !complete) return;
            uint32_t index = kNone;
            bool prototypeLike = false;
            for (uint32_t i = 0; i < keys.size(); ++i) {
                if (keys[i] == key) index = index == kNone ? i : kNone - 1;
                if (keys[i] == "constructor") prototypeLike = true;
            }
            if (index >= kNone - 1 || (write && prototypeLike)) return;
            uint32_t slot = index;
            if (index >= inlineSlots) slot = BRONZE_ABI_OBJ_OVERFLOW_SLOT + (index - inlineSlots);
            if (slot != index) return;
            if (commonSlot == kNone) {
                commonSlot = slot;
            } else if (slot != commonSlot) {
                return;
            }
            uint32_t shapeLo = 0, shapeSpan = 0;
            if (!familyOf(keys, inlineSlots, slot, inst.keyIndex, write, &shapeLo, &shapeSpan)) return;
            lo = std::min(lo, shapeLo);
            hi = std::max(hi, shapeLo + shapeSpan);
            covered += count;
        }
        if (commonSlot == kNone || lo > hi || covered * 100 < names.total * kDominantPercent) return;
        for (uint32_t c = lo; c <= hi; ++c) {
            const auto& fields = mod_.classFamilies[c].fields;
            if (commonSlot >= fields.size() || fields[commonSlot].keyIndex != inst.keyIndex ||
                (write && !fields[commonSlot].writable)) {
                return;
            }
        }
        inst.staticSlot = commonSlot;
        inst.staticSlotGuessed = true;
        inst.staticCellIndex = 0;
        inst.familyLo = lo;
        inst.familySpan = hi - lo;
        ++stats_.shapeFamily;
        if (claimedIcs_.insert(inst.icIndex).second) stats_.shapeExecutions += names.total;
    }

    bool familyOf(const std::vector<std::string_view>& keys, uint32_t inlineSlots, uint32_t slot, uint32_t keyIndex,
                  bool write, uint32_t* lo, uint32_t* span) const {
        // When a shape's inline capacity is below the overflow split, its
        // overflow keys jump to `BRONZE_ABI_OBJ_OVERFLOW_SLOT`, leaving slots
        // `inlineSlots .. BRONZE_ABI_OBJ_OVERFLOW_SLOT - 1` empty
        // (`ownNodesBySlot` in class_family.cpp).
        const size_t prefix =
            inlineSlots < BRONZE_ABI_OBJ_OVERFLOW_SLOT ? std::min<size_t>(keys.size(), inlineSlots) : keys.size();
        std::vector<uint32_t> slots;
        slots.reserve(prefix);
        for (size_t i = 0; i < prefix; ++i) {
            const auto it = keyIndex_.find(std::string(keys[i]));
            slots.push_back(it == keyIndex_.end() ? kNone : it->second);
        }
        return wpo::familyClaimFor(mod_, slots, slot, keyIndex, write, lo, span);
    }

    il::Module& mod_;
    const FeedbackPlan& plan_;
    FeedbackApplyStats& stats_;
    std::unordered_map<std::string, uint32_t> keyIndex_;
    std::unordered_set<uint32_t> claimedIcs_;
    std::unordered_set<uint32_t> chainedIcs_;
    std::set<ValueKey> hinted_;
};

}  // namespace

void applyFeedbackAccessors(il::Module& mod, const FeedbackPlan& plan, const AccessorResolver& resolve,
                            FeedbackApplyStats* stats) {
    FeedbackApplyStats local;
    FeedbackApplyStats& st = stats ? *stats : local;
    // One cell pair per site, shared by its copies as they share its cache
    // (direct_method_table.cpp, resolveAccessors).
    std::unordered_map<uint32_t, uint32_t> cellOf;
    std::unordered_set<uint32_t> refused;
    std::vector<std::string_view> keys;
    for (il::Function& fn : mod.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        const std::vector<uint8_t> observable = observableValues(fn);
        for (il::Block& block : fn.blocks) {
            for (il::Instruction& inst : block.instructions) {
                if (inst.op != il::Op::PropGet && inst.op != il::Op::PropSet) continue;
                // A definition never reaches a setter.
                if (inst.staticSlot != il::Instruction::kNoStaticSlot ||
                    inst.directTarget != il::Instruction::kNoDirectTarget || il::isDefineWrite(inst) ||
                    inst.operands.empty() || inst.operands[0] >= observable.size() ||
                    observable[inst.operands[0]] == 0 || inst.keyIndex >= mod.keyConstants.size()) {
                    continue;
                }
                const std::string& key = mod.keyConstants[inst.keyIndex];
                if (il::parseIndexKey(key)) continue;
                const auto it = plan.shapesByIc.find(inst.icIndex);
                if (it == plan.shapesByIc.end()) continue;
                const std::string* shape = it->second.dominant(kDominantPercent);
                // A shape of more keys than the profile lists (`,+N`) may
                // hold the key among the ones it leaves out: the guess is
                // made all the same, and the cell's publish, which resolves
                // the key on the receiver, refuses a receiver that has it
                // as its own (pixi.js's Sprite has 62 own keys, and every
                // accessor read on one met this).
                bool complete = false;
                if (shape == nullptr || !shapeKeys(*shape, keys, complete)) continue;
                // The key own on the receiver is A's and E's (a slot), never
                // an accessor of the prototype's.
                if (std::find(keys.begin(), keys.end(), key) != keys.end()) continue;
                // The class the prototype's constructor names; `^Name` (an
                // `Object.create` link in between) names none.
                std::string proto = shape->substr(0, shape->find('{'));
                if (const size_t hash = proto.find('#'); hash != std::string::npos) proto.resize(hash);
                if (proto.empty() || proto.front() == '^') continue;
                const bool setter = inst.op == il::Op::PropSet;
                const uint32_t target = resolve(proto, key, setter);
                if (target >= mod.functions.size()) {
                    if (refused.insert(inst.icIndex).second) ++st.accessorNone;
                    continue;
                }
                auto cell = cellOf.find(inst.icIndex);
                if (cell == cellOf.end()) {
                    cell = cellOf.emplace(inst.icIndex, mod.staticSiteCount).first;
                    mod.staticSiteCount += 2;
                    st.accessorExecutions += it->second.total;
                }
                inst.directTarget = target;
                inst.staticCellIndex = cell->second;
                ++(setter ? st.accessorSets : st.accessorGets);
            }
        }
    }
}

uint32_t addProfiledFamilies(il::Module& mod, const FeedbackPlan& plan) {
    // More than this and the walk every shape's stamp makes (class_family.cpp)
    // is paying for classes no hot site reads.
    constexpr size_t kMaxAdded = 128;
    std::unordered_map<std::string_view, uint32_t> keyIndex;
    for (uint32_t k = 0; k < mod.keyConstants.size(); ++k) keyIndex.emplace(mod.keyConstants[k], k);
    using Fields = std::vector<uint32_t>;
    auto begins = [](const Fields& whole, const Fields& head) {
        return head.size() <= whole.size() && std::equal(head.begin(), head.end(), whole.begin());
    };
    // Lexicographic order is the preorder of the lists' prefix tree: a list is
    // followed by the lists it begins, which is what a range of ids covers.
    std::set<Fields> found;
    std::vector<std::string_view> keys;
    for (const auto& [ic, names] : plan.shapesByIc) {
        (void)ic;
        std::vector<Fields> met;
        bool usable = true;
        for (const auto& [name, count] : names.counts) {
            if (count == 0 || name == "*other*") continue;
            bool complete = false;
            uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
            if (!shapeKeys(name, keys, complete, &inlineSlots)) {
                usable = false;
                break;
            }
            // The positions that are slots (familyOf): all of them, or those
            // before a capacity that splits the keys early.
            const size_t prefix =
                inlineSlots < BRONZE_ABI_OBJ_OVERFLOW_SLOT ? std::min<size_t>(keys.size(), inlineSlots) : keys.size();
            Fields fields;
            for (size_t i = 0; i < prefix && usable; ++i) {
                const auto it = keyIndex.find(keys[i]);
                // `constructor` is how a prototype looks from here, and a
                // prototype's writes must reach the runtime.
                usable = it != keyIndex.end() && keys[i] != "constructor";
                if (usable) fields.push_back(it->second);
            }
            if (!usable) break;
            met.push_back(std::move(fields));
        }
        if (!usable || met.size() < 2) continue;
        const Fields& head = *std::min_element(met.begin(), met.end(),
                                               [](const Fields& a, const Fields& b) { return a.size() < b.size(); });
        if (head.size() < 2 || !std::all_of(met.begin(), met.end(), [&](const Fields& f) { return begins(f, head); })) {
            continue;
        }
        found.insert(head);
    }
    uint32_t added = 0;
    for (const Fields& fields : found) {
        if (added == kMaxAdded) break;
        bool related = false;
        for (size_t c = 0; c + added < mod.classFamilies.size() && !related; ++c) {
            Fields have;
            for (const auto& f : mod.classFamilies[c].fields) have.push_back(f.keyIndex);
            related = !have.empty() && (begins(fields, have) || begins(have, fields));
        }
        if (related) continue;
        il::Module::ClassFamilyEntry entry;
        entry.name = "profile:" + mod.keyConstants[fields.front()] + ".." + mod.keyConstants[fields.back()];
        for (uint32_t k : fields) entry.fields.push_back({k, true});
        mod.classFamilies.push_back(std::move(entry));
        ++added;
    }
    return added;
}

void applyFeedbackClaimsAndHints(il::Module& mod, const FeedbackPlan& plan, FeedbackApplyStats* stats,
                                 std::unordered_set<uint32_t>* missedClaims) {
    FeedbackApplyStats local;
    ClaimWriter writer(mod, plan, stats ? *stats : local);
    for (il::Function& fn : mod.functions) {
        if (fn.isExternal || fn.blocks.empty()) continue;
        const std::vector<uint8_t> observable = observableValues(fn);
        for (il::Block& block : fn.blocks) {
            const std::vector<uint8_t> interior = runInteriors(block);
            for (size_t i = 0; i < block.instructions.size(); ++i) {
                il::Instruction& inst = block.instructions[i];
                if ((inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) && interior[i] == 0) {
                    writer.claim(inst, observable);
                    writer.chain(inst);
                    if (missedClaims != nullptr && writer.missed(inst)) missedClaims->insert(inst.icIndex);
                } else {
                    writer.hint(inst, observable);
                    writer.truth(inst, observable);
                }
            }
        }
    }
}

namespace {

// The keys the harvest saw hold nothing but plain objects (feedback_plan.h
// `harvestObjects`, `nonObjectKeys`): a key at an `objects` position of some
// receiver, never found or born holding anything else, and at such a position
// of EVERY receiver the caches held that has it. The last is what keeps a
// claim on one from refusing a layout the run used: a receiver whose node for
// the key the runtime did not see born (an object literal's) may be built
// boxed in the next run, and a cell or a family that requires an object slot
// would not admit it.
std::set<std::string> objectKeysOf(const FeedbackPlan& plan) {
    std::set<std::string> keys;
    std::vector<std::string_view> names;
    bool complete = false;
    for (const auto& [receiver, positions] : plan.harvestObjects) {
        if (!shapeKeys(receiver, names, complete)) continue;
        for (uint32_t i : positions) {
            if (i < names.size() && plan.nonObjectKeys.count(std::string(names[i])) == 0) keys.emplace(names[i]);
        }
    }
    std::set<std::string> receivers;
    for (const auto& [ic, counts] : plan.shapesByIc) {
        for (const auto& [name, count] : counts.counts) receivers.insert(name);
    }
    for (const std::string& receiver : receivers) {
        if (!shapeKeys(receiver, names, complete)) continue;
        const auto held = plan.harvestObjects.find(receiver);
        for (uint32_t i = 0; i < names.size(); ++i) {
            const auto it = keys.find(std::string(names[i]));
            if (it == keys.end()) continue;
            const bool kept = held != plan.harvestObjects.end() &&
                              std::binary_search(held->second.begin(), held->second.end(), i);
            if (!kept) keys.erase(it);
        }
    }
    return keys;
}

}  // namespace

void applyFeedbackObjectKeys(il::Module& mod, const FeedbackPlan& plan) {
    const std::set<std::string> keys = objectKeysOf(plan);
    mod.slotReprObjectFields.clear();
    for (uint32_t k = 0; k < mod.keyConstants.size(); ++k) {
        if (keys.count(mod.keyConstants[k]) != 0) mod.slotReprObjectFields.push_back(k);
    }
}

uint32_t applyFeedbackLayoutCells(il::Module& mod, const FeedbackPlan& plan, const std::vector<uint8_t>& hot,
                                  const std::unordered_set<uint32_t>& missedClaims, const DominanceOf& dominanceOf) {
    // By shape name: its entry in `mod.layoutCells`.
    std::unordered_map<std::string, size_t> cellOf;
    // A member or a family field under a key the harvest saw hold only plain
    // objects (`applyFeedbackObjectKeys`) is an object one.
    const auto heldObjects = [&](uint32_t keyIndex, uint32_t slot) {
        return slot < BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT &&
               std::find(mod.slotReprObjectFields.begin(), mod.slotReprObjectFields.end(), keyIndex) !=
                   mod.slotReprObjectFields.end();
    };
    // Whether `shape` (a whole shape's name) has the site's key at the
    // site's slot.
    const auto slotIn = [&](const std::string& shape, const il::Instruction& inst) {
        std::vector<std::string_view> keys;
        bool complete = false;
        uint32_t inlineSlots = BRONZE_ABI_OBJ_INLINE_SLOTS;
        if (inst.keyIndex >= mod.keyConstants.size() || !shapeKeys(shape, keys, complete, &inlineSlots) || !complete) {
            return false;
        }
        const std::string& key = mod.keyConstants[inst.keyIndex];
        for (uint32_t i = 0; i < keys.size(); ++i) {
            if (keys[i] != key) continue;
            const uint32_t slot = i >= inlineSlots ? BRONZE_ABI_OBJ_OVERFLOW_SLOT + (i - inlineSlots) : i;
            return slot == inst.staticSlot;
        }
        return false;
    };
    // The one shape a claimed site met, with the site's key at its slot; null
    // for a site that met none, several, or one its claim does not describe.
    const auto soleShape = [&](const il::Instruction& inst) -> const std::string* {
        if (inst.keyIndex >= mod.keyConstants.size() || missedClaims.count(inst.icIndex) != 0 ||
            plan.shapeVariantIcs.count(inst.icIndex) != 0) {
            return nullptr;
        }
        const auto it = plan.shapesByIc.find(inst.icIndex);
        if (it == plan.shapesByIc.end()) return nullptr;
        const std::string* sole = nullptr;
        for (const auto& [name, count] : it->second.counts) {
            if (count == 0) continue;
            if (sole != nullptr || name == "*other*") return nullptr;
            sole = &name;
        }
        return sole != nullptr && slotIn(*sole, inst) ? sole : nullptr;
    };
    // Whether the harvest saw `key` born and kept a Number on every shape
    // named `shape` its caches met, and nothing else under it anywhere
    // (feedback_plan.h `harvestDoubles`, `mixedKeys`): the member is then a
    // `number` one, which the runtime publishes the cell for only on a shape
    // that calls the slot a double. A mixed key is never one, since making it
    // eligible would split shapes the run's objects did not.
    const auto heldDoubles = [&](const std::string& shape, uint32_t keyIndex, uint32_t slot) {
        if (slot >= BRONZE_ABI_SHAPE_DOUBLE_SLOT_LIMIT || plan.mixedKeys.count(mod.keyConstants[keyIndex]) != 0) {
            return false;
        }
        const auto held = plan.harvestDoubles.find(shape);
        if (held == plan.harvestDoubles.end()) return false;
        std::vector<std::string_view> keys;
        bool complete = false;
        if (!shapeKeys(shape, keys, complete) || !complete) return false;
        for (uint32_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == mod.keyConstants[keyIndex]) {
                return std::binary_search(held->second.begin(), held->second.end(), i);
            }
        }
        return false;
    };
    uint32_t rewritten = 0;
    for (size_t f = 0; f < mod.functions.size(); ++f) {
        il::Function& fn = mod.functions[f];
        if (f >= hot.size() || hot[f] == 0 || fn.isExternal || fn.blocks.empty() || fn.isCoroutineBody()) continue;
        // One group per (receiver, claim): its sites, its tests, and the
        // shape they agree on (null once two disagree or one names none).
        struct Group {
            std::vector<il::Instruction*> sites;
            std::vector<il::BlockId> blocks;  // each site's
            std::vector<il::Instruction*> tests;
            const std::string* shape = nullptr;
            bool refused = false;
            // Every site is a STAMPED one that could be a cell's member.
            bool stamped = true;
        };
        std::map<std::tuple<il::ValueId, uint32_t, uint32_t, uint32_t>, Group> groups;
        // By receiver: the shapes its PROFILED sites met alone, and where.
        // A profiled site is one the harvested build ran through its cache
        // every time, so its ways are everything it met; a stamped one
        // (`FeedbackPlan::stampedIcs`) kept a guard of its own in that build
        // and filled its cache only where the guard missed.
        std::map<il::ValueId, std::vector<std::pair<il::BlockId, const std::string*>>> profiled;
        for (il::BlockId bi = 0; bi < fn.blocks.size(); ++bi) {
            for (il::Instruction& inst : fn.blocks[bi].instructions) {
                const bool site = (inst.op == il::Op::PropGet || inst.op == il::Op::PropSet) &&
                                  inst.staticSlot != il::Instruction::kNoStaticSlot;
                if ((!site && inst.op != il::Op::IsShape) || inst.operands.empty()) continue;
                const bool family = inst.familyLo != il::Instruction::kNoFamily;
                Group& g = groups[{inst.operands[0], family ? 0 : inst.staticCellIndex, inst.familyLo,
                                   family ? inst.familySpan : 0}];
                if (!site) {
                    g.tests.push_back(&inst);
                    continue;
                }
                g.sites.push_back(&inst);
                g.blocks.push_back(bi);
                const bool member = inst.directTarget == il::Instruction::kNoDirectTarget && !il::isDefineWrite(inst);
                const bool stamped = plan.stampedIcs.count(inst.icIndex) != 0;
                // A stamp that may latch an unfinished object (il.h
                // staticRegrow) is a member where the site's cache says
                // what it met; a profile's own such claim met two shapes.
                const std::string* shape = member && (!inst.staticRegrow || !stamped) ? soleShape(inst) : nullptr;
                if (shape == nullptr || (g.shape != nullptr && *g.shape != *shape)) g.refused = true;
                if (g.shape == nullptr) g.shape = shape;
                if (!member || !stamped) g.stamped = false;
                if (!stamped && shape != nullptr) profiled[inst.operands[0]].emplace_back(bi, shape);
            }
        }
        // A group of stamped sites takes the shape the receiver's profiled
        // sites met, where they all met one and one of them runs whenever
        // the stamped site does (its block is the site's or dominates it):
        // what the stamped site's own cache says is what missed its guard,
        // often one object met before the guard held. So a receiver read
        // through a class family's range, a cell of the whole-program
        // stamps' and a profile's claim is tested once, against the cell.
        std::function<bool(il::BlockId, il::BlockId)> dominates;
        for (auto& [key, g] : groups) {
            if (!g.stamped || g.sites.empty()) continue;
            const auto met = profiled.find(std::get<0>(key));
            if (met == profiled.end()) continue;
            const std::string* shape = met->second.front().second;
            bool one = true;
            for (const auto& [block, name] : met->second) one = one && *name == *shape;
            if (!one) continue;
            if (!dominates) dominates = dominanceOf(fn);
            bool all = true;
            for (size_t i = 0; i < g.sites.size() && all; ++i) {
                all = slotIn(*shape, *g.sites[i]) &&
                      std::any_of(met->second.begin(), met->second.end(), [&](const auto& at) {
                          return at.first == g.blocks[i] || dominates(at.first, g.blocks[i]);
                      });
            }
            if (!all) continue;
            g.shape = shape;
            g.refused = false;
        }
        for (auto& [key, g] : groups) {
            if (g.refused || g.shape == nullptr || g.sites.empty()) continue;
            auto [it, fresh] = cellOf.try_emplace(*g.shape, mod.layoutCells.size());
            if (fresh) {
                std::vector<std::string_view> keys;
                bool complete = false;
                shapeKeys(*g.shape, keys, complete);
                il::Module::LayoutCellEntry entry;
                entry.cell = mod.staticSiteCount++;
                entry.keyCount = static_cast<uint32_t>(keys.size());
                mod.layoutCells.push_back(std::move(entry));
            }
            il::Module::LayoutCellEntry& entry = mod.layoutCells[it->second];
            for (il::Instruction* inst : g.sites) {
                const bool write = inst->op == il::Op::PropSet;
                auto pair = std::find_if(entry.pairs.begin(), entry.pairs.end(),
                                         [&](const auto& p) { return p.keyIndex == inst->keyIndex; });
                if (pair == entry.pairs.end()) {
                    entry.pairs.push_back({inst->keyIndex, inst->staticSlot, write,
                                           heldDoubles(*g.shape, inst->keyIndex, inst->staticSlot),
                                           heldObjects(inst->keyIndex, inst->staticSlot)});
                } else {
                    pair->write = pair->write || write;
                }
            }
            for (std::vector<il::Instruction*>* list : {&g.sites, &g.tests}) {
                for (il::Instruction* inst : *list) {
                    inst->staticCellIndex = entry.cell;
                    inst->familyLo = il::Instruction::kNoFamily;
                    inst->familySpan = 0;
                    inst->staticRegrow = false;
                    ++rewritten;
                }
            }
        }
    }
    // A class family's field under an object key is an object one: the
    // runtime stamps a class only on a shape that agrees, field by field
    // (runtime/class_family.cpp), so every family claims one representation
    // for the key whatever site asks.
    for (il::Module::ClassFamilyEntry& family : mod.classFamilies) {
        for (size_t i = 0; i < family.fields.size(); ++i) {
            family.fields[i].object = heldObjects(family.fields[i].keyIndex, static_cast<uint32_t>(i));
        }
    }
    // The `number` members' keys are eligible for a double slot (slot_repr.h),
    // or no object of the run would have one. (The object keys are, all of
    // them, since `applyFeedbackObjectKeys`.)
    for (const il::Module::LayoutCellEntry& entry : mod.layoutCells) {
        for (const auto& p : entry.pairs) {
            if (p.number && std::find(mod.slotReprFields.begin(), mod.slotReprFields.end(), p.keyIndex) ==
                                mod.slotReprFields.end()) {
                mod.slotReprFields.push_back(p.keyIndex);
            }
        }
    }
    return rewritten;
}

}  // namespace bronze::spec
