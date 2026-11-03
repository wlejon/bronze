// Return summaries (solver.h `RetSummary`): which functions return a read off
// their own parameters, and that read on one call's arguments.

#include <algorithm>

#include "opt/wpo/solver.h"

namespace bronze::wpo {

namespace {

// How many reads a chain may take: `this.a.b[i]` is three.
constexpr size_t kMaxSteps = 3;

}  // namespace

const Solver::RetSummary& Solver::retSummary(uint32_t f) {
    if (summaries_.empty()) summaries_.resize(mod_.functions.size());
    RetSummary& s = summaries_[f];
    if (s.computed) return s;
    s.computed = true;
    const il::Function& fn = mod_.functions[f];
    if (fn.isExternal || fn.blocks.empty() || fn.isGenerator || fn.isCoroutineBody()) return s;
    if (fn.returnType != il::Type::Dynamic) return s;

    std::vector<const il::Instruction*> def(fn.valueCount, nullptr);
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.result != il::kNoValue && inst.result < def.size()) def[inst.result] = &inst;
        }
    }
    // A chain starts at the receiver or a source parameter the call binds
    // as it was passed: not the environment, the arguments object or a rest
    // array, which the call builds, and not a typed parameter.
    const il::ValueId self = thisParam(f);
    const size_t first = fn.firstSourceParam();
    const size_t rest = fn.hasRestParam ? fn.params.size() - 1 : fn.params.size();
    auto isBase = [&](il::ValueId v) {
        if (v >= fn.params.size() || fn.params[v].type != il::Type::Dynamic) return false;
        return v == self || (v >= first && v < rest);
    };

    std::vector<RetPath> paths;
    for (const auto& block : fn.blocks) {
        for (const auto& inst : block.instructions) {
            if (inst.op != il::Op::Ret) continue;
            if (inst.operands.empty() || inst.operands[0] == il::kNoValue) return s;
            RetPath path;
            il::ValueId v = inst.operands[0];
            while (!isBase(v)) {
                const il::Instruction* d = v < def.size() ? def[v] : nullptr;
                if (d == nullptr || d->type != il::Type::Dynamic || d->operands.empty() ||
                    path.steps.size() == kMaxSteps) {
                    return s;
                }
                RetStep step;
                if (d->op == il::Op::PropGet) {
                    step.key = d->keyIndex;
                } else if (d->op == il::Op::ElemGet && d->operands.size() == 2) {
                    step.elems = true;
                    step.idx = d->operands[1];
                } else {
                    return s;
                }
                path.steps.push_back(step);
                v = d->operands[0];
            }
            path.param = v;
            std::reverse(path.steps.begin(), path.steps.end());
            paths.push_back(std::move(path));
        }
    }
    if (paths.empty()) return s;
    s.paths = std::move(paths);
    s.valid = true;
    return s;
}

AVal Solver::readPaths(uint32_t f, const RetSummary& summary, const std::vector<AVal>& actual) {
    AVal out;
    for (const RetPath& path : summary.paths) {
        AVal v = actual[path.param];
        for (const RetStep& step : path.steps) {
            if (step.elems) {
                // The key is the callee's own value: the call reads again
                // when it grows, as the callee's read does.
                auto& users = fns_[f].users[step.idx];
                if (curSite_ != UINT32_MAX && std::find(users.begin(), users.end(), curSite_) == users.end()) {
                    users.push_back(curSite_);
                }
                AVal next;
                readElem(v, fns_[f].values[step.idx], next);
                v = std::move(next);
            } else {
                v = readProp(v, step.key);
            }
        }
        out.join(v);
    }
    return out;
}

}  // namespace bronze::wpo
