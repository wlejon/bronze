#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "types/env.h"
#include "types/type.h"

namespace bronze::types {

struct ModuleContext;
struct Scope;
struct FunctionAnalysisArgs;
struct FunctionOutcome;

// The incremental half of the call-graph fixpoint: a function body is walked
// again only when something it READ has changed.
//
// Every round of the fixpoint walks the whole program, and most of it reads
// exactly what it read the round before — a signature that did not widen, a
// field whose harvest did not move, a captured binding that holds what it held.
// Such a walk is a pure function of those reads, so its result is known: the
// observations it joins into the round's tables (`observedParams` and the
// rest, reset every round), the writes it makes to cells of the functions
// enclosing it, and what its body returns. Everything else a walk writes is a
// JOIN into a table that lives across rounds — the module bindings, the poison
// sets, the write audit — and a walk that reads what it read before writes
// there what it wrote before, which is already there.
//
// So each `analyzeFunction` call of a probe round is memoized, keyed on its
// function: the FACTS the walk read, each at the value it had when first read,
// and the three kinds of effect above. A later call whose inputs match and
// whose every fact still has its recorded value replays the effects instead of
// walking. That is the worklist: a function is dirty exactly when one of the
// facts it read moved, and the check is made at the point in the round where
// the full walk would have walked it, against the state the full walk would
// have seen there.
//
// A fact read inside a nested function is a fact its enclosing function read
// too, so a frame's reads are folded into the frame around it when it ends —
// except reads of the enclosing function's OWN cells, which are that
// function's business and not an input to it.
//
// `BRONZE_INFER_CHECK=1` walks every call the memo would have replayed and
// compares the two, which is how a read this file does not know about shows
// up: as a named internal error instead of a wrong type. `BRONZE_NO_INFER_MEMO=1`
// turns the memo off.
class InferMemo {
public:
    enum class FactKind : uint8_t {
        OuterName,  // a name resolved through the enclosing functions' cells
        Binding,    // ModuleContext::moduleBindings
        Field,      // the harvest's type and the audit's verdict for one field
        Function,   // a module function's signature
        Method,     // a class method's signature and poison
        Ctor,       // a constructor's signature and poison
    };
    enum class Contrib : uint8_t {
        FnParam,
        MethodParam,
        MethodShape,
        MethodReturn,
        MethodSkipped,
        CtorParam,
    };

    struct FactValue {
        Type t;
        uint64_t n = 0;
        friend bool operator==(const FactValue& a, const FactValue& b) {
            return a.t == b.t && a.n == b.n;
        }
    };
    struct Fact {
        uint64_t key = 0;
        FactValue value;
    };
    struct Contribution {
        Contrib kind = Contrib::FnParam;
        uint32_t index = 0;
        uint32_t param = 0;
        Type t;
        friend bool operator==(const Contribution& a, const Contribution& b) {
            return a.kind == b.kind && a.index == b.index && a.param == b.param && a.t == b.t;
        }
    };
    // A join into a cell of an enclosing function, `level` scopes out (1 is
    // the parent).
    struct OuterWrite {
        uint32_t name = 0;
        uint32_t level = 0;
        Type t;
        friend bool operator==(const OuterWrite& a, const OuterWrite& b) {
            return a.name == b.name && a.level == b.level && a.t == b.t;
        }
    };

    explicit InferMemo(bool check) : check_(check) {}

    bool recording() const { return !frames_.empty(); }

    // ---- reads (only while recording) ---------------------------------------

    // `level` is how many scopes out the name resolved, 1 for the parent, or 0
    // when no enclosing function's cells hold it.
    void noteOuterName(std::string_view name, uint32_t level, Type t);
    void noteBinding(std::string_view name, bool present, Type t);
    void noteField(ShapeClassId cls, std::string_view field, Type fieldType, bool clean);
    void noteFunction(uint32_t index, uint32_t version);
    void noteMethod(uint32_t index, uint32_t version, bool poisoned);
    void noteCtor(uint32_t index, uint32_t version, bool poisoned);

    // ---- effects -------------------------------------------------------------

    void noteOuterWrite(std::string_view name, uint32_t level, Type t);

    // One join into a round's observation tables, applied and (while
    // recording) remembered. Every such join in the flow pass goes through here.
    void contribute(ModuleContext& mod, Contrib kind, uint32_t index, uint32_t param, Type t);
    // The join itself.
    static void apply(ModuleContext& mod, const Contribution& c);

    // ---- the memo --------------------------------------------------------------

    using Walk = FunctionOutcome (*)(ModuleContext&, const FunctionAnalysisArgs&);
    FunctionOutcome analyze(ModuleContext& mod, const FunctionAnalysisArgs& args, Walk walk);

    uint64_t hits() const { return hits_; }
    uint64_t walks() const { return walks_; }
    uint64_t checked() const { return checked_; }

private:
    struct Frame {
        std::vector<Fact> facts;
        std::unordered_map<uint64_t, uint32_t> seen;
        std::vector<Contribution> contributions;
        std::vector<OuterWrite> outerWrites;
    };
    struct Entry {
        std::vector<Type> paramTypes;
        ShapeClassId thisClass = kNoShapeClass;
        std::vector<Fact> facts;
        std::vector<Contribution> contributions;
        std::vector<OuterWrite> outerWrites;
        Type returnType;
        // A nested body's walk starts from the cells its last walk this round
        // left (`ModuleContext::nestedCells`), and what it contributes on the
        // way to its fixpoint depends on where it starts, so the start is an
        // input like the parameters.
        bool hasStart = false;
        Env startCells;
        Env finalCells;
        uint64_t lastUse = 0;
    };

    uint32_t intern(std::string_view name);
    void note(uint64_t key, FactValue value);
    FactValue current(const ModuleContext& mod, const Scope* parent, const Fact& fact) const;
    bool matches(const ModuleContext& mod, const FunctionAnalysisArgs& args, const Entry& e) const;
    // Folds a finished frame's (or a replayed entry's) reads and effects into
    // the frame enclosing it.
    void foldIntoParent(const std::vector<Fact>& facts, const std::vector<Contribution>& contributions,
                        const std::vector<OuterWrite>& outerWrites);
    void replay(ModuleContext& mod, const FunctionAnalysisArgs& args, const Entry& e);
    // What differs between a replay and the walk it stood for, or empty.
    std::string compare(const Entry& e, const Frame& walked, Type walkedReturn,
                        const Env& walkedCells) const;
    uint64_t persistentStamp(const ModuleContext& mod) const;

    // Names are looked up by view, so noting a read allocates nothing.
    struct NameHash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>{}(s); }
    };

    bool check_ = false;
    std::vector<Frame> frames_;
    std::unordered_map<const void*, std::vector<Entry>> entries_;
    std::unordered_map<std::string, uint32_t, NameHash, std::equal_to<>> nameIds_;
    std::vector<std::string> names_;
    uint64_t clock_ = 0;
    uint64_t hits_ = 0;
    uint64_t walks_ = 0;
    uint64_t checked_ = 0;
};

}  // namespace bronze::types
