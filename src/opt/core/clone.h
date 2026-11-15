#pragma once

// Copying IL: a whole function into a new module slot (the speculation step
// makes S from G this way), and a set of blocks into a function with every
// definition renamed (inlining a callee's body, peeling or unswitching a
// loop).

#include <cstdint>
#include <string>
#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"

namespace bronze::opt {

// Old id -> new id, kNoValue / kNoBlock where unmapped.
struct CloneMap {
    std::vector<ValueId> values;
    std::vector<BlockId> blocks;

    ValueId value(ValueId v) const { return v < values.size() ? values[v] : kNoValue; }
    BlockId block(BlockId b) const { return b < blocks.size() ? blocks[b] : kNoBlock; }
    void mapValue(ValueId from, ValueId to);
    void mapBlock(BlockId from, BlockId to);
};

// Appends a copy of `module.functions[fnIndex]` named `name` and returns its
// index. The copy keeps every value and block id (the map, if asked for, is
// the identity over them), is neither exported nor the entry point, and is
// otherwise the same function: the caller sets `speculatedFrom` and strips or
// keeps what it needs. Module::functions is a deque, so references to other
// functions stay valid.
uint32_t cloneFunction(il::Module& module, uint32_t fnIndex, const std::string& name,
                       CloneMap* map = nullptr);

// Appends copies of `blocks` (of `src`) to `dst`, in the order given, each
// with a fresh block id and every value it defines (block parameters,
// results) renamed to a fresh id of `dst`. Inside the copies:
//   - a value defined in a copied block becomes its copy;
//   - any other value becomes `map.value(v)` when the caller seeded one (a
//     callee's parameters -> the call's arguments), and stays itself when not
//     (which is only meaningful when src and dst are the same function);
//   - a target or handler in the set becomes its copy; one outside it becomes
//     `map.block(b)` when seeded, and stays itself when not.
// `map` comes back holding every renaming made. `src` and `dst` may be the
// same function.
void cloneBlocks(const il::Function& src, const std::vector<BlockId>& blocks, il::Function& dst,
                 CloneMap& map);

// Deletes every `resume.point` of `fn` (a speculated copy of G has none) and
// answers how many.
size_t removeResumePoints(il::Function& fn);

}  // namespace bronze::opt
