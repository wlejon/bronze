// RE-ENTRY: the way back from a region's slow copy into its fast copy.
//
// `guard_region.h` ("RE-ENTRY") states the rule. This file emits the blocks:
// the tests a back edge of the slow copy has to pass, the conversion behind
// them, and the fast header's preheader, which the entry edge shares.

#include "lower/guard_region_emit.h"

namespace bronze::lower {

uint32_t emitReentry(std::vector<il::Block>& out, const ReentryBlocks& blocks, const il::Block& header,
                     const std::vector<uint8_t>& promoted, const std::vector<il::ValueId>& entryGuarded,
                     const std::vector<il::ValueId>& entryUnboxed, const std::vector<il::ValueId>& rawOf,
                     il::BlockId slowHeader, il::BlockId fastHeader, const std::function<il::ValueId()>& newValue) {
    // The header's parameters as the back edge passes them, boxed: the first
    // block of the chain takes them, and every block behind it reads them.
    il::Block& first = out[blocks.first()];
    std::vector<il::ValueId> passed;
    for (const il::BlockParam& p : header.params) {
        passed.push_back(newValue());
        first.params.push_back(il::BlockParam{passed.back(), p.type});
    }
    std::vector<il::ValueId> tested = entryGuarded;
    for (size_t k = 0; k < header.params.size(); ++k) {
        if (promoted[k] != 0) tested.push_back(passed[k]);
    }
    for (size_t t = 0; t < tested.size(); ++t) {
        il::Block& test = out[blocks.tests[t]];
        const il::ValueId cond = newValue();
        test.instructions.push_back(makeIsNumber(cond, tested[t]));
        test.instructions.push_back(
            makeBranch(cond, t + 1 < tested.size() ? blocks.tests[t + 1] : blocks.conv, slowHeader, passed));
    }
    il::Block& conv = out[blocks.conv];
    std::vector<il::ValueId> args;
    il::Block& pre = out[blocks.pre];
    std::vector<il::ValueId> received;
    for (size_t k = 0; k < header.params.size(); ++k) {
        if (promoted[k] != 0) {
            args.push_back(newValue());
            conv.instructions.push_back(makeRawUnbox(args.back(), passed[k]));
        } else {
            args.push_back(passed[k]);
        }
        received.push_back(newValue());
        pre.params.push_back(il::BlockParam{received.back(), promoted[k] != 0 ? il::Type::F64 : header.params[k].type});
    }
    // The doubles of the values tested on the entry edge are the preheader's
    // parameters, behind the header's: each way in unboxes them behind its own
    // tests (a raw unbox wants a test that dominates it), and the preheader's
    // names for them dominate the fast copy.
    for (il::ValueId v : entryUnboxed) {
        args.push_back(newValue());
        conv.instructions.push_back(makeRawUnbox(args.back(), v));
        pre.params.push_back(il::BlockParam{rawOf[v], il::Type::F64});
    }
    conv.instructions.push_back(makeJump(blocks.pre, std::move(args)));
    pre.instructions.push_back(makeJump(fastHeader, std::move(received)));
    return static_cast<uint32_t>(tested.size());
}

}  // namespace bronze::lower
