#pragma once

// Where each SSA value is defined and where it is used, and the small
// mutators every pass needs to make values and blocks.
//
// A DefUse is a snapshot like a Cfg: build it, query it, rebuild it after the
// edits that matter. `replaceAllUses` works on the function directly and needs
// none.

#include <cstdint>
#include <vector>

#include "il/il.h"
#include "opt/core/cfg.h"

namespace bronze::opt {

struct DefSite {
    enum class Kind : uint8_t {
        None,         // no definition (an id past valueCount, or one never defined)
        Param,        // function parameter `index`
        BlockParam,   // parameter `index` of `block`
        Instruction,  // the result of instruction `index` of `block`
    };
    Kind kind = Kind::None;
    BlockId block = kNoBlock;
    uint32_t index = 0;
};

struct UseSite {
    enum class Kind : uint8_t {
        Operand,  // instruction `inst`'s operand `slot`
        ThenArg,  // argument `slot` of the instruction's `target` (jump or br then)
        ElseArg,  // argument `slot` of a `br`'s `elseTarget`
    };
    BlockId block = kNoBlock;
    uint32_t inst = 0;
    Kind kind = Kind::Operand;
    uint32_t slot = 0;
};

class DefUse {
public:
    explicit DefUse(const il::Function& fn);

    size_t valueCount() const { return defs_.size(); }
    const DefSite& def(ValueId v) const;
    il::Type type(ValueId v) const;
    // Every use, in block then instruction order.
    const std::vector<UseSite>& uses(ValueId v) const;
    bool unused(ValueId v) const { return uses(v).empty(); }
    // The defining instruction, or nullptr for a parameter or block parameter.
    const il::Instruction* defInst(const il::Function& fn, ValueId v) const;
    // The defining instruction when it is `op`, else nullptr.
    const il::Instruction* defInstIs(const il::Function& fn, ValueId v, il::Op op) const;

private:
    std::vector<DefSite> defs_;
    std::vector<il::Type> types_;
    std::vector<std::vector<UseSite>> uses_;
};

// The value slot a use names, for rewriting it in place.
ValueId& useRef(il::Function& fn, const UseSite& use);

// Every value an instruction reads, operands and edge arguments together.
template <typename F>
void forEachUse(const il::Instruction& inst, F&& f) {
    for (ValueId v : inst.operands) f(v);
    for (ValueId v : inst.target.args) f(v);
    for (ValueId v : inst.elseTarget.args) f(v);
}

// Replaces every use of `from` with `to`, operands and edge arguments, and
// answers how many it rewrote. The caller keeps the types equal.
size_t replaceAllUses(il::Function& fn, ValueId from, ValueId to);
// The batch form: `map[v]`, when it is not kNoValue, replaces `v`. Chains
// (a -> b -> c) resolve to their end.
size_t replaceAllUses(il::Function& fn, const std::vector<ValueId>& map);

// A fresh value id.
ValueId newValue(il::Function& fn);
// A fresh, empty block appended to the function (its id is its index). The
// reference is invalidated by the next block added.
il::Block& newBlock(il::Function& fn);

}  // namespace bronze::opt
