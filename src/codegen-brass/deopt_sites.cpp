#include "codegen-brass/deopt_sites.h"

#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include "il/il_op.h"

namespace bronze::codegen {

namespace {

using Key = std::pair<uint32_t, uint32_t>;  // (G, resume id)

// What the guards of speculated function `s` that leave for each point test:
// the op that defines the condition of every branch into a block ending in
// `leave`, or "inlined <callee>" for an exit of an inlined body (its
// `resume.call` finished the callee before the caller's own leave).
void noteGuards(const il::Function& s, const std::vector<std::string>& names,
                std::map<Key, std::set<std::string>>& out) {
    std::unordered_map<il::ValueId, const il::Instruction*> defs;
    for (const il::Block& b : s.blocks) {
        for (const il::Instruction& inst : b.instructions) {
            if (inst.result != il::kNoValue) defs[inst.result] = &inst;
        }
    }
    std::unordered_map<il::BlockId, std::vector<const il::Instruction*>> into;
    for (const il::Block& b : s.blocks) {
        if (b.instructions.empty()) continue;
        const il::Instruction& t = b.instructions.back();
        if (t.op != il::Op::Branch || t.operands.empty()) continue;
        into[t.target.block].push_back(&t);
        into[t.elseTarget.block].push_back(&t);
    }
    for (const il::Block& b : s.blocks) {
        if (b.instructions.empty() || b.instructions.back().op != il::Op::Leave) continue;
        const il::Instruction& leave = b.instructions.back();
        std::set<std::string>& kinds = out[{leave.calleeIndex, static_cast<uint32_t>(leave.immI32)}];
        bool inlined = false;
        for (const il::Instruction& inst : b.instructions) {
            if (inst.op != il::Op::ResumeCall) continue;
            inlined = true;
            kinds.insert("inlined " + (inst.calleeIndex < names.size() ? names[inst.calleeIndex] : std::string("?")));
        }
        if (inlined) continue;
        auto it = into.find(b.id);
        if (it == into.end()) {
            kinds.insert("jump");
            continue;
        }
        for (const il::Instruction* br : it->second) {
            auto d = defs.find(br->operands[0]);
            kinds.insert(d == defs.end() ? std::string("param") : std::string(il::opName(d->second->op)));
        }
    }
}

std::string where(const il::Module& module, const bronze::Span& span) {
    std::string file = span.file < module.sourceFiles.size() ? module.sourceFiles[span.file] : std::string("?");
    if (const size_t slash = file.find_last_of('/'); slash != std::string::npos) file = file.substr(slash + 1);
    uint32_t line = 0;
    if ((span.begin != 0 || span.end != 0) && span.file < module.lineTables.size()) {
        line = module.lineTables[span.file].lineCol(span.begin).line;
    }
    return file + ":" + std::to_string(line);
}

}  // namespace

DeoptSites collectDeoptSites(const il::Module& module, const std::vector<std::string>& uniqueNames) {
    DeoptSites out;
    std::map<Key, std::set<std::string>> guards;
    std::set<uint32_t> generic;
    for (const il::Function& fn : module.functions) {
        if (fn.isSpeculated()) noteGuards(fn, uniqueNames, guards);
    }
    for (uint32_t f = 0; f < module.functions.size(); ++f) {
        const il::Function& g = module.functions[f];
        if (g.isSpeculated()) continue;
        // Blocks a later block jumps back to: a point at the top of one is
        // a loop's entry.
        std::set<il::BlockId> backTargets;
        for (const il::Block& b : g.blocks) {
            if (b.instructions.empty()) continue;
            const il::Instruction& t = b.instructions.back();
            if ((t.op == il::Op::Jump || t.op == il::Op::Branch) && t.target.block <= b.id) backTargets.insert(t.target.block);
            if (t.op == il::Op::Branch && t.elseTarget.block <= b.id) backTargets.insert(t.elseTarget.block);
        }
        std::map<uint32_t, std::string> lines;
        std::set<uint32_t> optimistic;
        for (const il::Block& b : g.blocks) {
            for (size_t i = 0; i < b.instructions.size(); ++i) {
                const il::Instruction& rp = b.instructions[i];
                if (rp.op != il::Op::ResumePoint || rp.immI32 < 0) continue;
                const auto id = static_cast<uint32_t>(rp.immI32);
                size_t next = i + 1;
                while (next < b.instructions.size() && b.instructions[next].op == il::Op::ResumePoint) ++next;
                std::string what;
                if (rp.keyIndex == il::kResumeOptimistic) {
                    optimistic.insert(id);
                    what = "optimistic ";
                }
                for (const std::string& k : guards[{f, id}]) what += (what.empty() ? "" : "/") + k;
                if (what.empty() || what == "optimistic ") what += "-";
                what += " -> ";
                what += next < b.instructions.size() ? il::opName(b.instructions[next].op) : "end";
                if (i == 0 && backTargets.count(b.id)) what += " (loop entry)";
                const bronze::Span span = next < b.instructions.size() ? b.instructions[next].span : rp.span;
                lines[id] = uniqueNames[f] + "\t" + std::to_string(id) + "\t" + what + "\t" + where(module, span);
            }
        }
        if (lines.empty()) continue;
        generic.insert(f);
        const auto rank = static_cast<uint32_t>(out.generics.size());
        out.generics.push_back(f);
        for (const auto& [id, line] : lines) {
            out.sites.push_back({f, id, rank, optimistic.count(id) != 0});
            out.text += line;
            out.text += '\n';
        }
    }
    for (uint32_t f = 0; f < module.functions.size(); ++f) {
        const il::Function& s = module.functions[f];
        if (s.isSpeculated() && generic.count(s.speculatedFrom)) out.retirable.push_back(f);
    }
    return out;
}

}  // namespace bronze::codegen
