#include "opt/wpo/heap.h"

#include <algorithm>

#include "il/key.h"

namespace bronze::wpo {

const char* objKindName(ObjKind kind) {
    switch (kind) {
        case ObjKind::Plain: return "plain";
        case ObjKind::Array: return "array";
        case ObjKind::Instance: return "instance";
        case ObjKind::Function: return "function";
        case ObjKind::Proto: return "proto";
        case ObjKind::Env: return "env";
        case ObjKind::TypedArray: return "typed-array";
        case ObjKind::Collection: return "collection";
        case ObjKind::Bound: return "bound";
        case ObjKind::Iterator: return "iterator";
        case ObjKind::Builtin: return "builtin";
        case ObjKind::Other: return "other";
    }
    return "?";
}

InstIndex::InstIndex(const il::Module& mod) {
    fnBlockBase_.reserve(mod.functions.size());
    uint32_t next = 0;
    for (uint32_t f = 0; f < mod.functions.size(); ++f) {
        fnBlockBase_.push_back(static_cast<uint32_t>(blockBase_.size()));
        for (const auto& block : mod.functions[f].blocks) {
            blockBase_.push_back(next);
            blockFn_.push_back(f);
            next += static_cast<uint32_t>(block.instructions.size());
        }
    }
    total_ = next;
}

InstIndex::Loc InstIndex::locate(uint32_t id) const {
    // The last block whose first id is <= id. Empty blocks share their
    // successor's first id, so the search takes the LAST such entry.
    auto it = std::upper_bound(blockBase_.begin(), blockBase_.end(), id);
    const size_t entry = static_cast<size_t>(it - blockBase_.begin()) - 1;
    Loc loc;
    loc.fn = blockFn_[entry];
    loc.block = static_cast<uint32_t>(entry - fnBlockBase_[loc.fn]);
    loc.inst = id - blockBase_[entry];
    return loc;
}

KeyTable::KeyTable(const il::Module& mod) : names_(mod.keyConstants) {
    byName_.reserve(names_.size());
    for (uint32_t i = 0; i < names_.size(); ++i) byName_.emplace(names_[i], i);
    indexCache_.assign(names_.size(), -1);
}

uint32_t KeyTable::intern(const std::string& name) {
    auto it = byName_.find(name);
    if (it != byName_.end()) return it->second;
    const uint32_t key = static_cast<uint32_t>(names_.size());
    names_.push_back(name);
    byName_.emplace(name, key);
    indexCache_.push_back(-1);
    return key;
}

bool KeyTable::isIndex(uint32_t key) {
    if (key >= names_.size()) return false;
    int8_t& cached = indexCache_[key];
    if (cached < 0) cached = il::parseIndexKey(names_[key]).has_value() ? 1 : 0;
    return cached == 1;
}

}  // namespace bronze::wpo
