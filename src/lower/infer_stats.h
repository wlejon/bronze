#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "support/source.h"
#include "types/result.h"

namespace bronze::lower {

struct CategoryStats {
    // Sites whose EMITTED code carries a claim: for a property access, one
    // stamped with a slot, which compiles to a guarded constant-offset access.
    uint32_t nativeCount = 0;
    // Property sites inference called monomorphic and nothing was emitted for.
    // Counted apart from `nativeCount` because the annotation changes no
    // instruction — both llvm_prop_get.cpp and llvm_prop_set.cpp cast it to
    // void — so folding the two together lets this census move by tens of
    // sites while the objects it is describing are byte-identical, which is
    // exactly the reading that misranks a lever. Property accesses only; the
    // other categories have no such annotation and leave it zero.
    uint32_t monoOnlyCount = 0;
    uint32_t dynamicCount = 0;
    // Of `nativeCount`, how many guard on a layout FAMILY rather than on
    // one shape's identity. Reported separately because the two are different
    // claims about the same slot: an identity site serves the one shape it
    // pinned, a family site serves every proven subclass of the class its
    // method was written in — which is the difference between claiming
    // `this.matrixWorld` in `Object3D.updateMatrixWorld` and declining it.
    uint32_t familySlotCount = 0;
    // Bail reason -> occurrences count
    std::map<std::string, uint32_t> bailReasons;

    uint32_t total() const { return nativeCount + monoOnlyCount + dynamicCount; }
};

// What one property site turned out to be, written in one place because the
// three facts are decided at different points in lowering it and only their
// combination says which column the site belongs in.
struct PropSiteVerdict {
    // The stamping step claimed a slot for this site, so its emitted code
    // guards a layout and loads at a constant offset (llvm_static_slot.h).
    bool emittedClaim = false;
    // That guard admits a class FAMILY rather than one shape's identity.
    bool familyGuard = false;
    // Inference proved the receiver's identity. On its own this reaches the IL
    // text and this census and nothing else — see `monoOnlyCount`.
    bool monomorphic = false;
};

struct ModuleInferStats {
    std::string moduleName;
    CategoryStats propertyAccesses;
    CategoryStats calls;
    CategoryStats elementOps;
};

class InferStatsCollector {
public:
    InferStatsCollector() = default;

    void setSourceSet(const SourceSet* sources) { sources_ = sources; }

    void recordPropertyAccess(uint16_t fileId, PropSiteVerdict verdict,
                              const std::string& bailReason = "");
    void recordCall(uint16_t fileId, bool isNative, const std::string& bailReason = "");
    void recordElementOp(uint16_t fileId, bool isNative, const std::string& bailReason = "");

    // The class-layout verdicts, whole-program rather than per module: a class
    // is proven or refused once, wherever its declaration was written.
    void recordClassLayouts(uint32_t proven, uint32_t familyMembers, uint32_t familyRoots,
                            const std::map<std::string, uint32_t>& refusals);

    // The whole-program field-type write audit: how many property names the
    // program writes at all, how many of those hold nothing but Numbers, why
    // the rest were refused, and how many READ SITES ended up cashing the proof
    // as a raw f64 load. The refusal histogram is the instrument this chunk
    // leaves behind: it is the list of what a name-global invariant costs on a
    // real library, ranked.
    void recordFieldAudit(const types::InferenceResult::FieldAuditReport& report,
                          uint32_t provenReadSites);

    // Which accessor reads on a module-scope object literal were proven to be
    // reads of the property the getter would have read. Named one by one and
    // not counted, because the interesting question about this proof is always
    // WHICH property it held for.
    void recordModuleLiteralAccessors(std::vector<std::string> forwards);

    // Which methods of a module-scope object literal a call site may run
    // instead of calling, and how many sites took each one. Both halves,
    // because they answer different questions: the shapes say what the
    // whole-program proof held for, and the counts say whether any call site
    // in this program was in a position to spend it.
    void recordModuleLiteralInlines(std::vector<std::string> shapes,
                                    const std::map<std::string, uint32_t>& sites);

    void recordMethodParams(const types::InferenceResult::MethodParamReport& report);

    // What the constructor-parameter join proved, and what stood in its way.
    // First link of the chain the field audit finishes: a parameter still
    // dynamic here is a `this.x = x` the audit will refuse.
    void recordCtorParams(const types::InferenceResult::CtorParamReport& report);

    std::string format() const;

private:
    ModuleInferStats& getOrCreateModule(uint16_t fileId);

    const SourceSet* sources_ = nullptr;
    // Map from normalized module name to ModuleInferStats for deterministic ordering
    std::map<std::string, ModuleInferStats> modules_;
    std::map<uint16_t, std::string> fileIdToName_;
    uint32_t classesProven_ = 0;
    // Of the proven ones, how many are in the layout-family forest, and how
    // many of those are its roots. A class is out of the forest when it has no
    // fields (its field list would be a prefix of every shape in the program).
    uint32_t classFamilyMembers_ = 0;
    uint32_t classFamilyRoots_ = 0;
    std::map<std::string, uint32_t> classRefusals_;
    types::InferenceResult::FieldAuditReport fieldAudit_;
    uint32_t fieldProvenReads_ = 0;
    std::vector<std::string> moduleLiteralForwards_;
    std::vector<std::string> moduleLiteralInlines_;
    std::map<std::string, uint32_t> moduleLiteralInlineSites_;
    types::InferenceResult::MethodParamReport methodParams_;
    types::InferenceResult::CtorParamReport ctorParams_;
};

}  // namespace bronze::lower
