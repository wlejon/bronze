#pragma once

// What every site rewrite of one speculated function shares: the module, the
// pair (G, S), G's resume points, the module's shape-cell table, the entry
// map and S's value types. Internal to the speculation step.

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "il/il.h"
#include "opt/speculate/resume_points.h"
#include "opt/speculate/speculate.h"

namespace bronze::opt::speculate {

// The module's shape cells (Module::shapeCells), deduplicated by key: the
// cell a key already has, or a new one appended.
class ShapeCells {
public:
    explicit ShapeCells(il::Module& module);
    uint32_t cellFor(const std::string& key);
    uint32_t added() const { return added_; }
    // The member cell (il.h Module::memberCells) a site with ONE receiver
    // shape uses: one per (kind, key, function, the shape the harvest named),
    // shared by every such site, so the tests of one receiver against it are
    // one value the redundancy pass numbers. Whatever shape the runtime
    // publishes into a cell resolves the key to the function (accessor_cell
    // .cpp), so sharing changes no answer; a site whose receiver is not the
    // published shape leaves, as it would at a cell of its own holding it.
    uint32_t memberCellFor(il::Module::MemberKind kind, uint32_t keyIndex, uint32_t callee, const std::string& shape);

private:
    il::Module& module_;
    std::unordered_map<std::string, uint32_t> byKey_;
    std::unordered_map<std::string, uint32_t> members_;
    uint32_t added_ = 0;
};

// THE ENTRY MAP: for each function of the module as lowering left it, the
// function its function objects' code is once entry is redirected — S for a
// speculated G, the function itself otherwise. The one answer to "which
// index does a direct call, a `create.func` or a `fn.code_is` name for f":
// the code a closure of f runs is S's (the driver rewrites every
// `create.func @G` to `@S`), so an identity test against G would never pass
// and a direct call to G would skip the speculated code.
class EntryMap {
public:
    explicit EntryMap(size_t functionCount) : entry_(functionCount) {
        for (size_t i = 0; i < functionCount; ++i) entry_[i] = static_cast<uint32_t>(i);
    }
    void setSpeculated(uint32_t generic, uint32_t speculated) { entry_[generic] = speculated; }
    uint32_t entryOf(uint32_t fn) const { return fn < entry_.size() ? entry_[fn] : fn; }
    bool isRedirected(uint32_t fn) const { return fn < entry_.size() && entry_[fn] != fn; }
    size_t size() const { return entry_.size(); }

private:
    std::vector<uint32_t> entry_;
};

struct RewriteContext {
    il::Module& module;
    uint32_t genericIndex;
    uint32_t speculatedIndex;
    const il::Function& generic;  // G, unchanged until its points go in
    il::Function& speculated;     // S, being rewritten
    ResumePoints& resume;
    ShapeCells& cells;
    const EntryMap& entries;
    // The type of every value of S by id; G's ids first (S keeps them), then
    // what the rewrites add (GuardBuilder keeps it current).
    std::vector<il::Type> types;
    SpeculateStats& stats;
    // A fresh inline-cache index for a generic read a rewrite adds (the
    // method call's callee fetch): appended past the module's count, never
    // renumbering one that exists.
    uint32_t newIcSite() { return module.icSiteCount++; }
};

// Every value's type by id, parameters, block parameters and results.
std::vector<il::Type> valueTypes(const il::Function& fn);

}  // namespace bronze::opt::speculate
