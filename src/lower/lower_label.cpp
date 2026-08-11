// Labels, and the one stack every `break` and `continue` searches
// (docs/0018 decision 3).
//
// A label is not a binding and produces no value: it names a statement so
// that a jump inside it can say which statement it means. So the whole of the
// feature is (a) recording the name on the jump target the labelled statement
// pushes, and (b) the search rules — which are three, and are what make
// `break` and `continue` different statements rather than two spellings of
// one:
//
//   - unlabelled `break`    -> the innermost LOOP OR SWITCH;
//   - unlabelled `continue` -> the innermost LOOP;
//   - labelled either       -> the entry carrying that label, and for
//                              `continue` that entry must be a loop.
//
// The third rule is why a labelled block is on the same stack as a loop: it
// is a legal `break` target and an illegal `continue` one, and only one stack
// can answer both questions in the same nesting order.

#include <string>
#include <vector>

#include "ast/assigned.h"
#include "lower/lowerer.h"

namespace bronze::lower {

std::string Lowerer::takePendingLabel() {
    std::string label;
    label.swap(pendingLabel_);
    return label;
}

const Lowerer::JumpTarget* Lowerer::findJumpTarget(const std::string& label, bool forContinue,
                                                   Span span) {
    if (label.empty()) {
        for (size_t i = jumpStack_.size(); i-- > 0;) {
            const JumpTarget& t = jumpStack_[i];
            if (t.kind == JumpKind::Loop) return &t;
            // A switch is breakable and is not an iteration statement, so a
            // `break` inside one leaves the switch while a `continue` inside
            // one goes on to the enclosing loop (ECMA-262 14.12, 14.9).
            if (!forContinue && t.kind == JumpKind::Switch) return &t;
        }
        diags_.error(span, forContinue ? "continue statement outside of a loop"
                                       : "break statement outside of a loop or switch");
        return nullptr;
    }
    for (size_t i = jumpStack_.size(); i-- > 0;) {
        const JumpTarget& t = jumpStack_[i];
        if (t.label != label) continue;
        if (forContinue && t.kind != JumpKind::Loop) {
            // ECMA-262 14.9.1 makes this an early error, and it is worth
            // naming rather than treating as "jump to the end": a `continue`
            // that reached a block's exit would be a `break` wearing the
            // wrong word.
            diags_.error(span, "continue label '" + label +
                                   "' does not name an enclosing loop (only a loop can be "
                                   "continued)");
            return nullptr;
        }
        return &t;
    }
    diags_.error(span, (forContinue ? "continue label '" : "break label '") + label +
                           "' does not name an enclosing labelled statement");
    return nullptr;
}

void Lowerer::emitJumpToTarget(const JumpTarget& target, il::BlockId block,
                               const std::vector<il::ValueId>& extraArgs, il::Function& ilFn) {
    auto args = collectEdgeArgs(target.vars, block, ilFn);
    for (il::ValueId extra : extraArgs) args.push_back(extra);
    il::Instruction jmpInst;
    jmpInst.op = il::Op::Jump;
    jmpInst.type = il::Type::Void;
    jmpInst.result = il::kNoValue;
    jmpInst.target = il::BlockTarget{.block = block, .args = std::move(args)};
    emitInst(ilFn, jmpInst);

    // Statements after the jump in the same clause are unreachable and have
    // nowhere to go; a dead block gives them a home the verifier accepts.
    il::BlockId deadBlock = createBlock(ilFn);
    setCurrentBlock(deadBlock);
}

bool Lowerer::lowerLabeledStmt(const ast::LabeledStmt* labeled, il::Function& ilFn) {
    // ECMA-262 14.13.1: a label may not shadow one already in scope, because
    // the inner `break lbl` would then be unable to name the outer statement
    // at all. Two SIBLING statements may share a label, which is why this
    // checks the enclosing labels and not every label in the function.
    for (const auto& active : labelStack_) {
        if (active == labeled->label) {
            diags_.error(labeled->span,
                         "duplicate label '" + labeled->label + "' (it is already in scope)");
            return false;
        }
    }
    if (!labeled->body) {
        diags_.error(labeled->span, "internal: a label with no statement");
        return false;
    }

    labelStack_.push_back(labeled->label);
    // A loop or a switch already builds an exit block and pushes a jump
    // target; handing it the label is the whole of labelling it. Anything
    // else needs an exit block of its own.
    const bool ownsTarget = dynamic_cast<const ast::WhileStmt*>(labeled->body.get()) ||
                            dynamic_cast<const ast::DoWhileStmt*>(labeled->body.get()) ||
                            dynamic_cast<const ast::ForStmt*>(labeled->body.get()) ||
                            dynamic_cast<const ast::ForOfStmt*>(labeled->body.get()) ||
                            dynamic_cast<const ast::ForInStmt*>(labeled->body.get()) ||
                            dynamic_cast<const ast::SwitchStmt*>(labeled->body.get());
    bool ok = false;
    if (ownsTarget) {
        pendingLabel_ = labeled->label;
        ok = lowerStmt(*labeled->body, ilFn);
        // A statement that did not claim the label is a drift between this
        // list and the statements that call takePendingLabel().
        pendingLabel_.clear();
    } else {
        ok = lowerLabeledBlock(labeled, ilFn);
    }
    labelStack_.pop_back();
    return ok;
}

bool Lowerer::lowerLabeledBlock(const ast::LabeledStmt* labeled, il::Function& ilFn) {
    // The exit block joins the fall-through off the end of the statement with
    // every `break lbl` inside it, so it takes a parameter per variable the
    // statement assigns — the same shape a loop's exit block has, and for the
    // same reason.
    const auto params = collectLoopParams(*labeled, ast::getAssignedNames(*labeled->body));
    std::vector<std::string> vars;
    for (const auto& p : params) vars.push_back(p.name);

    il::BlockId bExit = createBlock(ilFn);
    auto exitParamMap = addLoopBlockParams(params, bExit, ilFn);

    jumpStack_.push_back(
        JumpTarget{JumpKind::LabeledBlock, labeled->label, il::kNoBlock, il::kNoBlock, bExit,
                   vars, cleanupStack_.size(), cleanupStack_.size()});
    const bool ok = lowerStmt(*labeled->body, ilFn);
    jumpStack_.pop_back();
    if (!ok) return false;

    if (!currentBlockIsTerminated(ilFn)) {
        il::Instruction jmpInst;
        jmpInst.op = il::Op::Jump;
        jmpInst.type = il::Type::Void;
        jmpInst.result = il::kNoValue;
        jmpInst.target =
            il::BlockTarget{.block = bExit, .args = collectEdgeArgs(vars, bExit, ilFn)};
        emitInst(ilFn, jmpInst);
    }
    setCurrentBlock(bExit);
    bindLoopBlockParams(params, exitParamMap);
    return true;
}

}  // namespace bronze::lower
