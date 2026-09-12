#include <string>
#include <vector>

#include "ast/ast.h"
#include "lower/lowerer.h"

namespace bronze::lower {

// A top-level `throw` makes the current block terminated; one body DROPS the
// statements after it (lowerStmtList says why that is the language's answer),
// so the segmented form drops them too rather than lowering segments no call
// would ever reach — `main`'s pending-exception check after each call is what
// makes the calls after a throwing segment unreachable at run time.
bool Lowerer::lowerTopLevelSegments(const std::vector<const ast::Stmt*>& topLevelStmts,
                                    il::Function& mainFn) {
    // Sized in IL instructions — the only size lowering can see. One IL
    // instruction expands to ~50 LLVM instructions (measured on the three.js
    // bundle: the inline IC fast paths are most of it), so 800 IL is roughly
    // a 40k-instruction function: big enough that per-function pass overhead
    // stays noise, small enough that emission partitions balance dozens of
    // them evenly.
    constexpr size_t kSegmentIlInsts = 800;
    const size_t mainBlockIdx = currentBlockIdx_;
    size_t stmtIdx = 0;
    unsigned segNo = 0;
    while (stmtIdx < topLevelStmts.size()) {
        il::Function segFn;
        segFn.name = "main.seg" + std::to_string(segNo++);
        segFn.returnType = il::Type::Void;
        segFn.isStrict = strictCode_;
        segFn.blocks.push_back(il::Block{.id = 0});

        // The reset `main` itself got above, less what stays module-wide:
        // functionVarNames_ (a `var` nested in any top-level statement is
        // module-scoped wherever it is written), the env layout, strict mode.
        varBindings_.clear();
        activeVarMap_.clear();
        currentScopeDepth_ = 0;
        varDeclCounter_ = 0;
        jumpStack_.clear();
        scopeHasEnv_.clear();
        currentBlockIdx_ = 0;
        currentThisValue_ = il::kNoValue;
        functionEnvBase_ = 0;
        functionEnvScope_ = moduleEnvScope_;
        immutableEnvCache_.clear();
        cachedTypedElemGet_.reset();
        // The module record, loaded the way every module function loads it.
        currentEnvValue_ =
            moduleEnvScope_ != SIZE_MAX ? emitModuleEnvGet(segFn) : il::kNoValue;
        entryEnvValue_ = currentEnvValue_;

        while (stmtIdx < topLevelStmts.size()) {
            if (!lowerStmt(*topLevelStmts[stmtIdx], segFn)) return false;
            ++stmtIdx;
            if (currentBlockIsTerminated(segFn)) {
                stmtIdx = topLevelStmts.size();
                break;
            }
            size_t segInsts = 0;
            for (const auto& b : segFn.blocks) segInsts += b.instructions.size();
            if (segInsts >= kSegmentIlInsts) break;
        }

        if (!currentBlockIsTerminated(segFn)) {
            il::Instruction retInst;
            retInst.op = il::Op::Ret;
            retInst.type = il::Type::Void;
            emitInst(segFn, retInst);
        }

        // Appended AFTER the closures the segment's own statements appended,
        // so the index is taken here, not before the statements lowered.
        const uint32_t segIndex = static_cast<uint32_t>(ilModule_.functions.size());
        ilModule_.functions.push_back(std::move(segFn));

        currentBlockIdx_ = mainBlockIdx;
        il::Instruction call;
        call.op = il::Op::Call;
        call.type = il::Type::Void;
        call.result = il::kNoValue;
        call.calleeIndex = segIndex;
        emitInst(mainFn, call);
    }
    return true;
}

}  // namespace bronze::lower
