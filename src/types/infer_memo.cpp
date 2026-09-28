#include "types/infer_memo.h"

#include <algorithm>
#include <iterator>
#include <string>
#include <tuple>

#include "types/flow.h"

namespace bronze::types {
namespace {

// A fact's key: its kind in the top byte, then two 28-bit operands (an index,
// an interned name, a shape class).
constexpr uint64_t kOperandLimit = uint64_t{1} << 28;

uint64_t factKey(InferMemo::FactKind kind, uint64_t a, uint64_t b) {
    return (static_cast<uint64_t>(kind) << 56) | ((a & (kOperandLimit - 1)) << 28) |
           (b & (kOperandLimit - 1));
}
InferMemo::FactKind kindOf(uint64_t key) { return static_cast<InferMemo::FactKind>(key >> 56); }
uint32_t operandA(uint64_t key) { return static_cast<uint32_t>((key >> 28) & (kOperandLimit - 1)); }
uint32_t operandB(uint64_t key) { return static_cast<uint32_t>(key & (kOperandLimit - 1)); }

// Where a name resolves through the cells of the functions enclosing a body
// whose parent scope is `parent`: 1 for the parent, 0 for nowhere. The same
// walk `FlowAnalyzer::lookup` makes past the body's own scope.
std::pair<uint32_t, Type> resolveOuter(const Scope* parent, std::string_view name) {
    uint32_t level = 1;
    for (const Scope* p = parent; p != nullptr; p = p->parent, ++level) {
        if (const auto it = p->cells.find(name); it != p->cells.end()) return {level, it->second};
    }
    return {0, Type::never()};
}

Scope* scopeAt(const FunctionAnalysisArgs& args, uint32_t level) {
    Scope* s = args.parent;
    for (uint32_t i = 1; i < level && s != nullptr; ++i) s = s->parent;
    return s;
}

// The function a call analyses: its site for a nested function, its
// declaration for a module function, and one shared key for the top level.
const void* keyOf(const ModuleContext& mod, const FunctionAnalysisArgs& args) {
    static const int kTopLevel = 0;
    if (args.site != nullptr) return args.site;
    if (args.moduleIndex != kNoFunctionIndex) return mod.functions[args.moduleIndex].decl;
    return &kTopLevel;
}

// The cells a nested body's walk starts from, or null when it starts empty.
const Env* startOf(const ModuleContext& mod, const FunctionAnalysisArgs& args) {
    if (args.parent == nullptr || args.site == nullptr) return nullptr;
    const auto it = mod.nestedCells.find(args.site);
    return it != mod.nestedCells.end() ? &it->second : nullptr;
}

// A function is analysed with a handful of different inputs in one round (its
// enclosing body's own fixpoint walks it once per iteration), so a few entries
// are kept per function rather than one.
constexpr size_t kEntriesPerFunction = 4;

template <typename T, typename Less>
std::vector<T> sortedUnique(std::vector<T> v, Less less) {
    std::sort(v.begin(), v.end(), less);
    v.erase(std::unique(v.begin(), v.end(),
                        [&](const T& a, const T& b) { return !less(a, b) && !less(b, a); }),
            v.end());
    return v;
}

bool contributionLess(const InferMemo::Contribution& a, const InferMemo::Contribution& b) {
    return std::make_tuple(static_cast<int>(a.kind), a.index, a.param, a.t.bitsForOrdering()) <
           std::make_tuple(static_cast<int>(b.kind), b.index, b.param, b.t.bitsForOrdering());
}

bool outerWriteLess(const InferMemo::OuterWrite& a, const InferMemo::OuterWrite& b) {
    return std::make_tuple(a.name, a.level, a.t.bitsForOrdering()) <
           std::make_tuple(b.name, b.level, b.t.bitsForOrdering());
}

}  // namespace

uint32_t InferMemo::intern(std::string_view name) {
    const auto it = nameIds_.find(name);
    if (it != nameIds_.end()) return it->second;
    const auto id = static_cast<uint32_t>(names_.size());
    names_.emplace_back(name);
    nameIds_.emplace(names_.back(), id);
    return id;
}

void InferMemo::note(uint64_t key, FactValue value) {
    Frame& f = frames_.back();
    // The FIRST read is the one the walk's course depended on; a later read of
    // the same fact saw what the walk itself (or a function nested in it) had
    // made of it since.
    if (f.seen.emplace(key, static_cast<uint32_t>(f.facts.size())).second) {
        f.facts.push_back(Fact{key, value});
    }
}

void InferMemo::noteOuterName(std::string_view name, uint32_t level, Type t) {
    note(factKey(FactKind::OuterName, intern(name), 0), FactValue{t, level});
}

void InferMemo::noteBinding(std::string_view name, bool present, Type t) {
    note(factKey(FactKind::Binding, intern(name), 0), FactValue{t, present ? 1u : 0u});
}

void InferMemo::noteField(ShapeClassId cls, std::string_view field, Type fieldType, bool clean) {
    note(factKey(FactKind::Field, cls, intern(field)), FactValue{fieldType, clean ? 1u : 0u});
}

void InferMemo::noteFunction(uint32_t index, uint32_t version) {
    note(factKey(FactKind::Function, index, 0), FactValue{Type::never(), version});
}

void InferMemo::noteMethod(uint32_t index, uint32_t version, bool poisoned) {
    note(factKey(FactKind::Method, index, 0),
         FactValue{Type::never(), (uint64_t{version} << 1) | (poisoned ? 1u : 0u)});
}

void InferMemo::noteCtor(uint32_t index, uint32_t version, bool poisoned) {
    note(factKey(FactKind::Ctor, index, 0),
         FactValue{Type::never(), (uint64_t{version} << 1) | (poisoned ? 1u : 0u)});
}

void InferMemo::noteOuterWrite(std::string_view name, uint32_t level, Type t) {
    frames_.back().outerWrites.push_back(OuterWrite{intern(name), level, t});
}

void ModuleContext::contribute(InferMemo::Contrib kind, uint32_t index, uint32_t param, Type t) {
    if (memo != nullptr) {
        memo->contribute(*this, kind, index, param, t);
    } else {
        InferMemo::apply(*this, InferMemo::Contribution{kind, index, param, t});
    }
}

void InferMemo::apply(ModuleContext& mod, const Contribution& c) {
    switch (c.kind) {
        case Contrib::FnParam: {
            Type& slot = mod.functions[c.index].observedParams[c.param];
            slot = join(slot, c.t);
            break;
        }
        case Contrib::MethodParam: {
            Type& slot = mod.methods.methods()[c.index].observedParams[c.param];
            slot = join(slot, c.t);
            break;
        }
        case Contrib::MethodShape: {
            Type& slot = mod.methods.methods()[c.index].observedParamShapes[c.param];
            slot = join(slot, c.t);
            break;
        }
        case Contrib::MethodReturn: {
            Type& slot = mod.methods.methods()[c.index].observedReturn;
            slot = join(slot, c.t);
            break;
        }
        case Contrib::MethodSkipped:
            mod.methods.methods()[c.index].sawSkippedDynamicArg[c.param] = true;
            break;
        case Contrib::CtorParam: {
            Type& slot = mod.ctors.ctors()[c.index].observedParams[c.param];
            slot = join(slot, c.t);
            break;
        }
    }
}

void InferMemo::contribute(ModuleContext& mod, Contrib kind, uint32_t index, uint32_t param, Type t) {
    const Contribution c{kind, index, param, t};
    apply(mod, c);
    if (recording()) frames_.back().contributions.push_back(c);
}

InferMemo::FactValue InferMemo::current(const ModuleContext& mod, const Scope* parent,
                                        const Fact& fact) const {
    const uint32_t a = operandA(fact.key);
    switch (kindOf(fact.key)) {
        case FactKind::OuterName: {
            const auto [level, t] = resolveOuter(parent, names_[a]);
            return FactValue{t, level};
        }
        case FactKind::Binding: {
            const auto it = mod.moduleBindings.find(names_[a]);
            if (it == mod.moduleBindings.end()) return FactValue{Type::dynamic(), 0};
            return FactValue{it->second, 1};
        }
        case FactKind::Field: {
            const std::string& field = names_[operandB(fact.key)];
            const Type t = mod.result->classLayouts.fieldTypeOf(a, field);
            return FactValue{t, mod.fieldAudit.numberCleanFor(a, field) ? 1u : 0u};
        }
        case FactKind::Function:
            return FactValue{Type::never(), mod.functions[a].version};
        case FactKind::Method: {
            const auto& m = mod.methods.methods()[a];
            return FactValue{Type::never(),
                             (uint64_t{m.version} << 1) | (mod.methodPoison.poisons(a) ? 1u : 0u)};
        }
        case FactKind::Ctor: {
            const auto& c = mod.ctors.ctors()[a];
            return FactValue{Type::never(), (uint64_t{c.version} << 1) |
                                                (mod.ctorPoison.poisons(c.className) ? 1u : 0u)};
        }
    }
    return FactValue{Type::dynamic(), ~uint64_t{0}};
}

bool InferMemo::matches(const ModuleContext& mod, const FunctionAnalysisArgs& args,
                        const Entry& e) const {
    if (e.thisClass != args.thisClass || e.paramTypes != args.paramTypes) return false;
    const Env* start = startOf(mod, args);
    if (e.hasStart != (start != nullptr)) return false;
    if (start != nullptr && !(e.startCells == *start)) return false;
    for (const Fact& fact : e.facts) {
        if (!(current(mod, args.parent, fact) == fact.value)) return false;
    }
    return true;
}

void InferMemo::foldIntoParent(const std::vector<Fact>& facts,
                               const std::vector<Contribution>& contributions,
                               const std::vector<OuterWrite>& outerWrites) {
    if (frames_.empty()) return;
    Frame& parent = frames_.back();
    for (Fact fact : facts) {
        if (kindOf(fact.key) == FactKind::OuterName && fact.value.n != 0) {
            // Resolved in the parent's own scope: the parent's business.
            if (fact.value.n == 1) continue;
            --fact.value.n;
        }
        if (parent.seen.emplace(fact.key, static_cast<uint32_t>(parent.facts.size())).second) {
            parent.facts.push_back(fact);
        }
    }
    parent.contributions.insert(parent.contributions.end(), contributions.begin(),
                                contributions.end());
    for (OuterWrite w : outerWrites) {
        if (w.level == 1) continue;
        --w.level;
        parent.outerWrites.push_back(w);
    }
}

void InferMemo::replay(ModuleContext& mod, const FunctionAnalysisArgs& args, const Entry& e) {
    for (const OuterWrite& w : e.outerWrites) {
        if (Scope* s = scopeAt(args, w.level)) {
            Type& slot = s->cells[names_[w.name]];
            slot = join(slot, w.t);
        }
    }
    for (const Contribution& c : e.contributions) apply(mod, c);
    if (args.parent != nullptr && args.site != nullptr) mod.nestedCells[args.site] = e.finalCells;
    foldIntoParent(e.facts, e.contributions, e.outerWrites);
}

uint64_t InferMemo::persistentStamp(const ModuleContext& mod) const {
    return mod.persistentWrites + mod.fieldAudit.observeChanges() + mod.methodPoison.version() +
           mod.ctorPoison.version() + mod.ctorShapes.size();
}

std::string InferMemo::compare(const Entry& e, const Frame& walked, Type walkedReturn,
                               const Env& walkedCells) const {
    if (!(e.returnType == walkedReturn)) return "the return type";
    if (!(e.finalCells == walkedCells)) return "the captured-variable cells";
    const auto memoized = sortedUnique(e.contributions, contributionLess);
    const auto fresh = sortedUnique(walked.contributions, contributionLess);
    if (memoized != fresh) {
        // Name the first observation only one side makes.
        std::vector<Contribution> diff;
        std::set_symmetric_difference(memoized.begin(), memoized.end(), fresh.begin(), fresh.end(),
                                      std::back_inserter(diff), contributionLess);
        std::string what = "the call-site observations";
        if (!diff.empty()) {
            const Contribution& c = diff.front();
            const bool walkedOnly = std::binary_search(fresh.begin(), fresh.end(), c, contributionLess);
            what += " (kind " + std::to_string(static_cast<int>(c.kind)) + ", target " +
                    std::to_string(c.index) + ", param " + std::to_string(c.param) + ", " + c.t.str() +
                    (walkedOnly ? ", only when walked)" : ", only when replayed)");
        }
        return what;
    }
    if (sortedUnique(e.outerWrites, outerWriteLess) !=
        sortedUnique(walked.outerWrites, outerWriteLess)) {
        return "the writes to enclosing cells";
    }
    return {};
}

FunctionOutcome InferMemo::analyze(ModuleContext& mod, const FunctionAnalysisArgs& args, Walk walk) {
    const void* key = keyOf(mod, args);
    std::vector<Entry>& slots = entries_[key];
    Entry* hit = nullptr;
    for (Entry& e : slots) {
        if (matches(mod, args, e)) {
            hit = &e;
            break;
        }
    }
    if (hit != nullptr && !check_) {
        ++hits_;
        hit->lastUse = ++clock_;
        replay(mod, args, *hit);
        return FunctionOutcome{hit->returnType, true};
    }

    ++walks_;
    const uint64_t stampBefore = check_ && hit != nullptr ? persistentStamp(mod) : 0;
    Entry fresh;
    if (const Env* start = startOf(mod, args)) {
        fresh.hasStart = true;
        fresh.startCells = *start;
    }
    frames_.emplace_back();
    const FunctionOutcome out = walk(mod, args);
    Frame frame = std::move(frames_.back());
    frames_.pop_back();
    if (!out.ok || mod.failed) return out;

    fresh.paramTypes = args.paramTypes;
    fresh.thisClass = args.thisClass;
    fresh.returnType = out.returnType;
    if (args.parent != nullptr && args.site != nullptr) {
        if (const auto it = mod.nestedCells.find(args.site); it != mod.nestedCells.end()) {
            fresh.finalCells = it->second;
        }
    }

    if (hit != nullptr) {
        ++checked_;
        std::string what = compare(*hit, frame, out.returnType, fresh.finalCells);
        if (what.empty() && persistentStamp(mod) != stampBefore) what = "a program-wide table";
        if (!what.empty()) {
            mod.failed = true;
            mod.diags->error(args.span, "internal: incremental type inference would have replayed '" +
                                            args.qualifiedName + "' but walking it changes " + what);
            return FunctionOutcome{Type::dynamic(), false};
        }
    }

    fresh.facts = frame.facts;
    fresh.contributions = frame.contributions;
    fresh.outerWrites = frame.outerWrites;
    fresh.lastUse = ++clock_;
    foldIntoParent(frame.facts, frame.contributions, frame.outerWrites);
    if (hit != nullptr) {
        *hit = std::move(fresh);
    } else if (slots.size() < kEntriesPerFunction) {
        slots.push_back(std::move(fresh));
    } else {
        auto oldest = std::min_element(slots.begin(), slots.end(), [](const Entry& x, const Entry& y) {
            return x.lastUse < y.lastUse;
        });
        *oldest = std::move(fresh);
    }
    return out;
}

}  // namespace bronze::types
