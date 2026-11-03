#pragma once

// The whole-program abstract interpreter (AOT only): an inclusion-based
// (Andersen-style) points-to analysis over the final `il::Module`, with the
// call graph discovered on the fly.
//
// VALUES are `AVal`s (value.h), one per SSA value of each reachable function,
// and grow monotonically. The HEAP is one abstract object per allocation site
// (heap.h), field-sensitive by key, except that a class instance is one object
// per CONSTRUCTOR: a class's shape is its constructor's, and per-`new`-site
// instances only made receiver sets as large as the program's count of `new`
// sites. A FUNCTION's parameters receive the join of every call's arguments
// (context-insensitive: one set of parameters per IL function), its returns
// flow back to every caller, and a call through a value calls every function
// that value can be. A method call splits its receivers by target: each
// target runs with only the receivers whose lookup finds it as `this`.
//
// The solver is a worklist of INSTRUCTIONS. An instruction runs again when a
// value it uses grows, when a heap location it read changes (`depend` /
// `notify`), or, for a call, when a callee's return grows. Monotone transfers
// over a finite lattice make it a fixpoint.
//
// CONTROL is followed too: a block runs once control can reach it, and a
// branch whose condition the lattice knows (a constant, a `typeof` of a value
// of one type compared with a string) takes one edge. Booleans are two bits
// (value.h) for that reason.
//
// Two MODES share the solver (SolverOptions::predict). A proof holds for every
// run of the program and is what the census reports as `wpo`. A prediction is
// what a fast path may assume behind a runtime guard: escaped objects keep the
// fields the program gave them, a name only the global object could define is
// undefined, and an unknown receiver dispatches to nothing the program shows.
//
// Context sensitivity is not built in, but the place for it is: the state of
// a function is `FnState`, looked up by function index alone. Keying that table
// by (function, context) — a receiver allocation site, for 1-object
// sensitivity — and choosing the context where a call binds its callee
// (`bindFunction`) is the whole of the change.
//
// THE OPEN WORLD. Anything the analysis cannot see is `topObj` (value.h). A
// tracked object that reaches it ESCAPES (`escapeObj`): unknown code may have
// read it, so every value reachable from its own properties escapes too; it may
// have written it, so a read of any key may give an unknown value and the
// object may gain keys; a function that escapes is called with unknown
// receiver and arguments, and what it returns escapes. From then on the object
// is one of the objects `topObj` stands for: a value that takes it on holds
// `topObj` instead (`joinTracked`), except a method's receiver. A method call
// on an unknown receiver therefore also runs every method of the program the
// chain of an escaped object holds, with that object as `this`. A prototype is
// NOT made to escape by an instance that does: natives are assumed to reach an
// escaped object's inherited behaviour only through the protocol keys the
// host calls by name (`handleEvent`, `then`, `toJSON`, `toString`,
// `valueOf`), whose functions escape when stored.

#include <chrono>
#include <cstdint>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "il/il.h"
#include "opt/wpo/builtins.h"
#include "opt/wpo/heap.h"
#include "opt/wpo/value.h"

namespace bronze::wpo {

struct SolverOptions {
    // The names the host defines (`--host-globals`, the native manifest's
    // roots): reading one is the open world, except `console` and
    // `performance`, whose behaviour the catalog knows.
    std::vector<std::string> hostGlobals;
    // A points-to set that grows past this widens: its objects escape and the
    // value becomes unknown. A safety valve for the solver's running time,
    // not a precision knob.
    size_t setCap = 1024;
    // PREDICTIONS instead of proofs: what a guarded fast path may assume.
    // Escaped objects stay in the sets that reach them and keep the fields
    // the program wrote (unknown code is assumed not to write them), and a
    // name only the global object could define (`name.resolve`) is
    // undefined. Every fact this mode adds must be one a runtime guard
    // checks before code relies on it.
    bool predict = false;
};

struct SolverStats {
    uint64_t transfers = 0;
    uint32_t widenings = 0;
    double millis = 0;
};

// The pseudo-keys a heap dependency can name beside a real key.
inline constexpr uint32_t kKeyElems = 0xFFFFFF00u;
inline constexpr uint32_t kKeySym = 0xFFFFFF01u;
inline constexpr uint32_t kKeyProto = 0xFFFFFF02u;
inline constexpr uint32_t kKeyEnv = 0xFFFFFF03u;
inline constexpr uint32_t kKeyFlags = 0xFFFFFF04u;  // escaped / anyKey / deleted
inline constexpr uint32_t kKeyAny = 0xFFFFFF05u;    // any own property

class Solver {
public:
    Solver(const il::Module& mod, SolverOptions opts);
    void run();

    // ---- results, valid after run() ----
    const il::Module& module() const { return mod_; }
    const InstIndex& insts() const { return insts_; }
    KeyTable& keys() { return keys_; }
    const KeyTable& keys() const { return keys_; }
    const BuiltinCatalog& catalog() const { return catalog_; }
    size_t objectCount() const { return objs_.size(); }
    const AObj& object(ObjId id) const { return objs_[id]; }
    bool reachable(uint32_t fn) const { return fns_[fn].reachable; }
    bool blockLive(uint32_t fn, uint32_t block) const {
        const auto& live = fns_[fn].live;
        return block < live.size() && live[block] != 0;
    }
    const SolverOptions& options() const { return opts_; }
    const AVal& value(uint32_t fn, il::ValueId v) const { return fns_[fn].values[v]; }
    const AVal& returned(uint32_t fn) const { return fns_[fn].ret; }
    bool calledWithUnknown(uint32_t fn) const { return fns_[fn].calledTop; }
    const SolverStats& stats() const { return stats_; }
    const std::string& reason(uint32_t index) const { return reasons_[index]; }
    // A short human name for an abstract object: `new Vector3@f`,
    // `Vector3.prototype`, `{}@f`, `fn Vector3.dot`.
    std::string describe(ObjId id) const;

    // Non-recording queries for the census and the shape pass, valid after
    // run(). The value a read of `key` gives on `recv`, with the getters it
    // would run folded in from their returns.
    AVal queryProp(const AVal& recv, uint32_t key);
    // The IL functions a callee value can enter, and whether it can also be
    // something unknown or a builtin.
    struct Callees {
        std::vector<uint32_t> fns;
        bool unknown = false;
        bool builtin = false;
        // Every builtin callee only stores or looks up its arguments (a
        // collection's set/add/get, an array push, a log): an object passed
        // to one is kept, never changed, and no user code runs.
        bool builtinKeepsOnly = true;
    };
    Callees callees(const AVal& callee) const;
    // The key a method-call IC site names, for `method.slot.fn`.
    uint32_t methodKeyOfIc(uint32_t ic) const;
    uint32_t key(const char* name) { return keys_.intern(name); }
    // The prototype a builtin path names, allocated on first use.
    ObjId builtinObj(const std::string& path);

private:
    struct FnState {
        bool reachable = false;
        bool calledTop = false;
        bool retEscapes = false;
        std::vector<AVal> values;
        AVal ret;
        // What the function returns other than its own receiver: the part of
        // `ret` a `new` of it yields besides the instance it allocated.
        AVal foreignRet;
        // A return gives `new` its own instance: the receiver, a primitive,
        // or no value. A function every return of which is another object
        // (a factory called with `new`) never yields the instance.
        bool newGivesInstance = false;
        // For each `super(...)` result on the function's own receiver, what
        // the base constructor returned other than that receiver.
        std::unordered_map<il::ValueId, AVal> superForeign;
        // The call instructions (global ids) that read `ret`.
        std::unordered_set<uint32_t> callers;
        // The blocks control can reach (Solver::markLive).
        std::vector<uint8_t> live;
        // For each value, the instructions (global ids) that use it.
        std::vector<std::vector<uint32_t>> users;
    };
    struct DepKey {
        uint32_t obj;
        uint32_t key;
        bool operator==(const DepKey& o) const { return obj == o.obj && key == o.key; }
    };
    struct DepHash {
        size_t operator()(const DepKey& k) const {
            return std::hash<uint64_t>()((static_cast<uint64_t>(k.obj) << 32) ^ k.key);
        }
    };
    struct DepFn {
        uint32_t obj;
        uint32_t key;
        uint32_t site;
        bool operator==(const DepFn& o) const { return obj == o.obj && key == o.key && site == o.site; }
    };
    struct DepFnHash {
        size_t operator()(const DepFn& k) const {
            return std::hash<uint64_t>()(((static_cast<uint64_t>(k.obj) << 32) ^ k.key) *
                                             0x9E3779B97F4A7C15ull ^
                                         k.site);
        }
    };
    // What the class / literal pre-pass learned about one creation site.
    struct SiteInfo {
        std::vector<uint32_t> definiteProtoKeys;  // sorted
        bool extendFollows = false;               // class.extend links its prototype
        bool literalProto = false;                // an object.proto sets it
    };
    struct CallArgs {
        std::vector<AVal> args;
        // The elements of a spread argument list, standing for any number of
        // arguments after `args`.
        const AVal* spread = nullptr;
    };

    // ---- solver.cpp: the driver and the instruction transfers ----
    void prepass();
    void markReachable(uint32_t fn);
    void markLive(uint32_t fn, uint32_t block);
    AVal typeOfValue(const AVal& v);
    // The worklist is of instructions: one runs again when a value it uses,
    // a heap location it read, or a callee's return changes.
    void enqueueInst(uint32_t site);
    void enqueueUsers(uint32_t fn, il::ValueId v);
    void enqueueCallers(uint32_t fn);
    void visit(uint32_t site);
    bool transfer(uint32_t fn, uint32_t block, uint32_t index, const il::Instruction& inst);
    bool setResult(const il::Instruction& inst, const AVal& v, bool keepEscaped = false);
    // Joins `from` into a value of IL type `type`, which keeps only that
    // type's primitives. An escaped object in `from` arrives as `topObj`
    // unless `keepEscaped` (a method's receiver, so its body still knows
    // which escaped objects it runs on).
    bool mergeInto(AVal& into, const AVal& from, il::Type type, bool keepEscaped = false);
    bool joinTracked(AVal& into, const AVal& from);
    void flowTarget(const il::BlockTarget& target, bool& changed);
    const AVal& op(const il::Instruction& inst, size_t i) const;
    ObjId alloc(uint32_t site, uint32_t tag, ObjKind kind);
    AVal createFunction(const il::Instruction& inst, uint32_t site);
    uint32_t internReason(const std::string& text);
    void widen(AVal& v);

    // ---- solver_props.cpp: the heap ----
    void depend(ObjId obj, uint32_t key);
    void notify(ObjId obj, uint32_t key);
    AVal readProp(const AVal& recv, uint32_t key);
    void readKey(ObjId obj, uint32_t key, const AVal& self, AVal& out, int depth);
    void readPrimProp(uint16_t prims, uint32_t key, AVal& out);
    void readBuiltinMember(ObjId obj, uint32_t key, AVal& out, bool& absent);
    void readElem(const AVal& recv, const AVal& idx, AVal& out);
    void readAll(const ObjSet& starts, AVal& out, bool enumerableOnly = false);
    // The keys a computed key with no string, symbol or object in it names:
    // "0" standing for every number, then "undefined", "null", "true",
    // "false". Empty when the key may be anything else.
    std::vector<uint32_t> primKeys(const AVal& idx);
    void writeProp(const AVal& recv, uint32_t key, const AVal& val, bool define);
    void writeField(ObjId obj, uint32_t key, const AVal& val);
    void writeElems(ObjId obj, const AVal& val);
    void writeElem(const AVal& recv, const AVal& idx, const AVal& val);
    void defineAccessor(const AVal& recv, uint32_t key, const AVal& getter, const AVal& setter);
    // Calls every setter of `key` on the chain from `obj`; true when the
    // lookup certainly ends at an accessor, so the write makes no own key.
    bool callSetters(ObjId obj, uint32_t key, const AVal& self, const AVal& val, int depth);
    void callAllSetters(ObjId obj, const AVal& self, const AVal& val, int depth);
    AVal envWalk(const AVal& env, uint32_t depth);
    AVal envGet(const AVal& env, uint32_t depth, uint32_t index);
    void envSet(const AVal& env, uint32_t depth, uint32_t index, const AVal& val);
    void escape(const AVal& v, uint32_t reason);
    // Escapes the functions in `v`: what a key a protocol reads may hold.
    void escapeCallables(const AVal& v, uint32_t reason);
    void escapeObj(ObjId id, uint32_t reason);
    void markFlag(ObjId obj, bool AObj::*flag);
    bool isNumericKey(uint32_t key);
    bool isImplicitKey(uint32_t key) const;

    // ---- solver_calls.cpp: calls ----
    AVal callValue(const AVal& callee, const AVal& self, const CallArgs& args, int depth = 0);
    AVal bindFunction(uint32_t fn, ObjId fnObj, const AVal& self, const CallArgs& args);
    void bindTop(uint32_t fn, ObjId fnObj);
    AVal construct(const AVal& callee, const CallArgs& args, int depth = 0);
    // `r`'s prototype chain holds an object the program made.
    bool userChain(ObjId r);
    AVal superCall(const AVal& base, const AVal& self, const CallArgs& args, AVal& foreign);
    il::ValueId thisParam(uint32_t fn) const;
    AVal directCall(const il::Instruction& inst);
    AVal methodCall(const il::Instruction& inst, const AVal& recv, const CallArgs& args);
    AVal coroStart(const il::Instruction& inst);
    ObjId restArray(uint32_t fn);
    AVal coerceThis(const AVal& self, bool strict) const;
    void escapeArgs(const AVal& self, const CallArgs& args, uint32_t reason);

    // ---- solver_summary.cpp: returns read off the arguments ----
    // A function each of whose returns is its receiver or a parameter, or a
    // chain of reads from one (`return this`, `return arr[i]`,
    // `return this.items[k]`). A call of it gives that chain read on what
    // the CALL passed rather than on the join of every caller's arguments,
    // which is what keeps `v.copy(a).add(b)` on `v` and
    // `pick(materials)` on the materials: the one piece of context
    // sensitivity the solver has. The function's own values stay joined.
    struct RetStep {
        bool elems = false;            // an element read, at `idx` (a value of the function)
        uint32_t key = 0;              // else a named read of `key`
        il::ValueId idx = il::kNoValue;
    };
    struct RetPath {
        il::ValueId param = 0;
        std::vector<RetStep> steps;    // in read order
    };
    struct RetSummary {
        bool computed = false;
        bool valid = false;
        std::vector<RetPath> paths;
    };
    const RetSummary& retSummary(uint32_t fn);
    // The summary's reads on `actual`, the value each parameter of this call
    // is bound to.
    AVal readPaths(uint32_t fn, const RetSummary& summary, const std::vector<AVal>& actual);

    // ---- solver_builtins.cpp: builtin behaviour ----
    AVal globalValue(uint32_t key);
    AVal callBuiltin(Effect effect, ObjId fnObj, const AVal& self, const CallArgs& args,
                     bool isConstruct, int depth);
    AVal arg(const CallArgs& args, size_t i) const;
    AVal elemsOf(const AVal& v);
    AVal iterValues(const AVal& v);
    AVal newArray(uint32_t tag, const AVal& elems, ObjKind kind = ObjKind::Array);
    AVal callback(const AVal& fn, const AVal& self, std::vector<AVal> args, int depth);

    const il::Module& mod_;
    SolverOptions opts_;
    const BuiltinCatalog& catalog_;
    InstIndex insts_;
    KeyTable keys_;
    std::deque<AObj> objs_;
    std::vector<FnState> fns_;
    std::deque<uint32_t> worklist_;
    std::vector<uint8_t> queued_;
    // Difference propagation for the reads that dominate the solve: a visit
    // reads the receivers it has not read and those whose heap facts changed
    // (`dirtyObjs_`), and all of them only when something else under the read
    // changed (a prototype, a getter's return: `rereadAll_`).
    struct MethodLookup {
        ObjSet seen;
        std::unordered_map<ObjId, ObjSet> byTarget;
        bool unknownTarget = false;
    };
    ObjSet unreadReceivers(const ObjSet& recv, const ObjSet& seen);
    std::vector<uint8_t> rereadAll_;
    std::unordered_map<uint32_t, ObjSet> dirtyObjs_;
    std::unordered_map<uint32_t, ObjSet> readRecv_;
    std::unordered_map<uint32_t, MethodLookup> lookups_;
    // Every escaped object, in the order it escaped, and the method calls on
    // an unknown receiver, which dispatch over them (methodCall). Those
    // calls run again whenever the worklist drains with objects escaped
    // since they last ran.
    std::vector<ObjId> escapedList_;
    std::set<uint32_t> topCallSites_;
    size_t escapedSeen_ = 0;
    std::unordered_map<uint64_t, ObjId> sites_;
    std::unordered_map<uint32_t, ObjId> builtinObjs_;
    std::unordered_map<uint32_t, ObjId> fnSingletons_;
    std::unordered_map<uint32_t, ObjId> restArrays_;
    // The one instance object of each constructor, by function index.
    std::unordered_map<uint32_t, ObjId> instances_;
    std::unordered_map<uint32_t, SiteInfo> siteInfo_;
    std::unordered_map<uint32_t, uint32_t> methodKeyByIc_;
    std::vector<RetSummary> summaries_;
    std::unordered_set<std::string> hostGlobals_;
    std::unordered_map<DepKey, std::vector<uint32_t>, DepHash> readers_;
    std::unordered_set<DepFn, DepFnHash> readerSeen_;
    std::vector<std::string> reasons_;
    std::unordered_map<std::string, uint32_t> reasonIndex_;
    AVal moduleEnv_;
    std::vector<uint32_t> moduleEnvReaders_;
    std::vector<uint32_t> implicitKeys_;
    SolverStats stats_;

    // The visit in progress: whose values an instruction reads, which
    // instruction is the allocation site, and whether effects are recorded.
    uint32_t curFn_ = UINT32_MAX;
    uint32_t curSite_ = UINT32_MAX;
    bool recording_ = false;
};

}  // namespace bronze::wpo
