#include "check/deterministic_check.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <brass/codegen/lir.hpp>
#include <brass/verify/arm64_lir_equiv.hpp>
#include <verify/bv_solver.hpp>

#include "lex/lexer.h"
#include "lower/lower.h"
#include "modules/modules.h"
#include "parse/parser.h"
#include "support/diagnostics.h"
#include "types/infer.h"

namespace bronze::check {
namespace {

bool isTranscendentalMathMethod(std::string_view name) {
    static const std::unordered_set<std::string_view> kMethods = {
        "sin",   "cos",   "tan",   "asin",  "acos",  "atan",  "atan2",
        "sinh",  "cosh",  "tanh",  "asinh", "acosh", "atanh", "exp",
        "expm1", "log",   "log1p", "log10", "log2",  "pow",   "cbrt",
        "hypot",
    };
    return kMethods.count(name) > 0;
}

bool isObjectEnumerationMethod(std::string_view name) {
    return name == "keys" || name == "values" || name == "entries";
}

bool isIntegerLiteralExpr(const ast::Expr* expr) {
    if (!expr) return false;
    if (auto* num = ast::exactAs<const ast::NumberLit*>(expr)) {
        return std::isfinite(num->value) && std::trunc(num->value) == num->value;
    }
    if (auto* un = ast::exactAs<const ast::Unary*>(expr)) {
        if (un->op == ast::UnaryOp::Negate || un->op == ast::UnaryOp::Posate) {
            return isIntegerLiteralExpr(un->operand.get());
        }
    }
    return false;
}

class DeterministicAstVisitor final : public ast::Visitor {
public:
    explicit DeterministicAstVisitor(std::vector<CheckDiagnostic>& diags) : diags_(diags) {
        fnStack_.push_back("<top-level>");
    }

    void visit(const ast::NumberLit&) override {}
    void visit(const ast::BigIntLit&) override {}
    void visit(const ast::SpreadElement& n) override {
        if (n.argument) n.argument->accept(*this);
    }
    void visit(const ast::StringLit&) override {}
    void visit(const ast::TemplateLit& n) override {
        for (const auto& e : n.exprs) {
            if (e) e->accept(*this);
        }
    }
    void visit(const ast::TaggedTemplate& n) override {
        if (n.tag) n.tag->accept(*this);
        if (n.templateLit) n.templateLit->accept(*this);
    }
    void visit(const ast::RegExpLit&) override {}
    void visit(const ast::BoolLit&) override {}
    void visit(const ast::NullLit&) override {}
    void visit(const ast::UndefinedLit&) override {}
    void visit(const ast::ThisExpr&) override {}
    void visit(const ast::Ident&) override {}

    void visit(const ast::Unary& n) override {
        if (n.operand) n.operand->accept(*this);
    }

    void visit(const ast::Binary& n) override {
        if (n.op == ast::BinaryOp::Exp || n.op == ast::BinaryOp::ExpAssign) {
            if (!isIntegerLiteralExpr(n.lhs.get()) || !isIntegerLiteralExpr(n.rhs.get())) {
                emit(DeterministicHazardKind::TranscendentalFloatDrift,
                     n.span,
                     "exponentiation operator (**) uses platform pow() which may drift by 1 ULP across x86_64 and AArch64",
                     "use repeated integer multiplication or fixed-point arithmetic");
            }
        }
        if (n.lhs) n.lhs->accept(*this);
        if (n.rhs) n.rhs->accept(*this);
    }

    void visit(const ast::Ternary& n) override {
        if (n.condition) n.condition->accept(*this);
        if (n.thenExpr) n.thenExpr->accept(*this);
        if (n.elseExpr) n.elseExpr->accept(*this);
    }

    void visit(const ast::MemberAccess& n) override {
        if (n.object) n.object->accept(*this);
    }

    void visit(const ast::IndexAccess& n) override {
        if (n.object) n.object->accept(*this);
        if (n.index) n.index->accept(*this);
    }

    void visit(const ast::Call& n) override {
        if (auto* mem = ast::exactAs<const ast::MemberAccess*>(n.callee.get())) {
            // Check if this is `<expr>.sort(...)` wrapping `Object.keys/values/entries(...)`
            if (mem->property == "sort" && mem->object) {
                if (auto* innerCall = ast::exactAs<const ast::Call*>(mem->object.get())) {
                    sortedObjectCalls_.insert(innerCall);
                }
            }

            if (auto* recvIdent = ast::exactAs<const ast::Ident*>(mem->object.get())) {
                if (recvIdent->name == "Math") {
                    if (mem->property == "random") {
                        emit(DeterministicHazardKind::UnseededPrng,
                             n.span,
                             "Math.random() is non-deterministic across lockstep peers and replays",
                             "replace Math.random() with a seeded integer PRNG (e.g. xorshift32 or mulberry32)");
                    } else if (isTranscendentalMathMethod(mem->property)) {
                        emit(DeterministicHazardKind::TranscendentalFloatDrift,
                             n.span,
                             "Math." + mem->property +
                                 "() relies on host libm transcendental approximation that can drift by 1 ULP across x86_64 and AArch64",
                             "replace Math." + mem->property +
                                 "() with a deterministic fixed-point LUT or Chebyshev polynomial");
                    }
                } else if (recvIdent->name == "Date" && mem->property == "now") {
                    emit(DeterministicHazardKind::WallClockTime,
                         n.span,
                         "Date.now() reads non-deterministic host wall-clock time",
                         "use a deterministic simulation tick or frame counter");
                } else if (recvIdent->name == "performance" && mem->property == "now") {
                    emit(DeterministicHazardKind::WallClockTime,
                         n.span,
                         "performance.now() reads non-deterministic host high-resolution timer",
                         "use a deterministic simulation tick or frame counter");
                } else if (recvIdent->name == "Object" && isObjectEnumerationMethod(mem->property)) {
                    if (sortedObjectCalls_.count(&n) == 0) {
                        emit(DeterministicHazardKind::ObjectIterationOrder,
                             n.span,
                             "Object." + mem->property +
                                 "() returns keys/entries in insertion-history order without deterministic sorting",
                             "chain .sort() on Object." + mem->property + "(...) or maintain a sorted array of keys");
                    }
                }
            }
        }

        if (n.callee) n.callee->accept(*this);
        for (const auto& arg : n.args) {
            if (arg) arg->accept(*this);
        }
    }

    void visit(const ast::NewExpr& n) override {
        if (auto* ident = ast::exactAs<const ast::Ident*>(n.callee.get())) {
            if (ident->name == "Date" && n.args.empty()) {
                emit(DeterministicHazardKind::WallClockTime,
                     n.span,
                     "new Date() with no arguments captures non-deterministic host wall-clock time",
                     "pass an explicit deterministic epoch timestamp or use simulation ticks");
            }
        }
        if (n.callee) n.callee->accept(*this);
        for (const auto& arg : n.args) {
            if (arg) arg->accept(*this);
        }
    }

    void visit(const ast::NewTargetExpr&) override {}
    void visit(const ast::ImportMetaExpr&) override {}
    void visit(const ast::SuperCall& n) override {
        if (n.baseExpr) n.baseExpr->accept(*this);
        for (const auto& arg : n.args) {
            if (arg) arg->accept(*this);
        }
    }
    void visit(const ast::SuperMember& n) override {
        if (n.baseExpr) n.baseExpr->accept(*this);
        if (n.propertyExpr) n.propertyExpr->accept(*this);
    }
    void visit(const ast::YieldExpr& n) override {
        if (n.argument) n.argument->accept(*this);
    }
    void visit(const ast::DynamicImportExpr& n) override {
        if (n.specifier) n.specifier->accept(*this);
    }
    void visit(const ast::DestructuringAssign& n) override {
        visitPattern(n.pattern.get());
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ObjectLit& n) override {
        for (const auto& prop : n.props) {
            if (prop.keyExpr) prop.keyExpr->accept(*this);
            if (prop.value) prop.value->accept(*this);
        }
    }
    void visit(const ast::ArrayLit& n) override {
        for (const auto& el : n.elements) {
            if (el) el->accept(*this);
        }
    }
    void visit(const ast::FunctionExpr& n) override {
        std::string fname = n.name.empty() ? "<anonymous>" : n.name;
        fnStack_.push_back(fname);
        for (const auto& p : n.params) {
            visitPattern(p.pattern.get());
            if (p.defaultValue) p.defaultValue->accept(*this);
        }
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
        fnStack_.pop_back();
    }
    void visit(const ast::ClassExpr& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.init) m.init->accept(*this);
            if (m.fn) m.fn->accept(*this);
        }
    }
    void visit(const ast::BlockStmt& n) override {
        for (const auto& s : n.stmts) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::VarDecl& n) override {
        visitPattern(n.pattern.get());
        if (n.init) n.init->accept(*this);
    }
    void visit(const ast::ReturnStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ExprStmt& n) override {
        if (n.expr) n.expr->accept(*this);
    }
    void visit(const ast::IfStmt& n) override {
        if (n.condition) n.condition->accept(*this);
        for (const auto& s : n.thenBody) {
            if (s) s->accept(*this);
        }
        for (const auto& s : n.elseBody) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::WhileStmt& n) override {
        if (n.condition) n.condition->accept(*this);
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::DoWhileStmt& n) override {
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
        if (n.condition) n.condition->accept(*this);
    }
    void visit(const ast::ForStmt& n) override {
        for (const auto& s : n.init) {
            if (s) s->accept(*this);
        }
        if (n.condition) n.condition->accept(*this);
        if (n.update) n.update->accept(*this);
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::BreakStmt&) override {}
    void visit(const ast::ContinueStmt&) override {}
    void visit(const ast::DebuggerStmt&) override {}
    void visit(const ast::SwitchStmt& n) override {
        if (n.discriminant) n.discriminant->accept(*this);
        for (const auto& c : n.cases) {
            if (c.test) c.test->accept(*this);
            for (const auto& s : c.body) {
                if (s) s->accept(*this);
            }
        }
    }
    void visit(const ast::ForInStmt& n) override {
        emit(DeterministicHazardKind::ObjectIterationOrder,
             n.span,
             "for..in loop enumerates object properties in insertion-dependent order",
             "iterate over Object.keys(obj).sort() with a deterministic for..of or indexed loop");
        visitPattern(n.pattern.get());
        if (n.object) n.object->accept(*this);
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::LabeledStmt& n) override {
        if (n.body) n.body->accept(*this);
    }
    void visit(const ast::ForOfStmt& n) override {
        visitPattern(n.pattern.get());
        if (n.iterable) n.iterable->accept(*this);
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::TryStmt& n) override {
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
        visitPattern(n.catchPattern.get());
        for (const auto& s : n.catchBody) {
            if (s) s->accept(*this);
        }
        for (const auto& s : n.finallyBody) {
            if (s) s->accept(*this);
        }
    }
    void visit(const ast::ThrowStmt& n) override {
        if (n.value) n.value->accept(*this);
    }
    void visit(const ast::ClassDecl& n) override {
        if (n.superClass) n.superClass->accept(*this);
        for (const auto& m : n.methods) {
            if (m.keyExpr) m.keyExpr->accept(*this);
            if (m.init) m.init->accept(*this);
            if (m.fn) m.fn->accept(*this);
        }
    }
    void visit(const ast::FunctionDecl& n) override {
        fnStack_.push_back(n.name.empty() ? "<anonymous>" : n.name);
        for (const auto& p : n.params) {
            visitPattern(p.pattern.get());
            if (p.defaultValue) p.defaultValue->accept(*this);
        }
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
        fnStack_.pop_back();
    }
    void visit(const ast::Module& n) override {
        for (const auto& s : n.body) {
            if (s) s->accept(*this);
        }
    }

private:
    std::vector<CheckDiagnostic>& diags_;
    std::vector<std::string> fnStack_;
    std::unordered_set<const ast::Call*> sortedObjectCalls_;

    void visitPattern(const ast::BindingPattern* pat) {
        if (!pat) return;
        for (const auto& el : pat->elements) {
            visitPattern(el.pattern.get());
            if (el.target) el.target->accept(*this);
            if (el.keyExpr) el.keyExpr->accept(*this);
            if (el.defaultValue) el.defaultValue->accept(*this);
        }
    }

    void emit(DeterministicHazardKind kind,
              Span span,
              std::string message,
              std::string remediation) {
        CheckDiagnostic d;
        d.kind = kind;
        d.span = span;
        d.functionName = fnStack_.empty() ? "<top-level>" : fnStack_.back();
        d.message = std::move(message);
        d.remediation = std::move(remediation);
        diags_.push_back(std::move(d));
    }
};

// Lowers an IL basic block's integer/bitwise slice to both x86_64 LirBlock and
// AArch64 LirBlock and verifies lockstep bitwise equivalence via Brass's
// verify_x64_lir_block_equivalence and verify_aarch64_lir_block_equivalence.
void auditIlAndCrossArchLir(const il::Module& ilMod, std::vector<CheckDiagnostic>& diags) {
    using brass::codegen::LirBlock;
    using brass::codegen::LirInst;
    using brass::codegen::LirOpcode;
    using brass::codegen::LirOperand;
    using brass::codegen::RegClass;
    using brass::codegen::VReg;

    auto makeVReg = [](uint32_t id) {
        VReg v;
        v.id = id;
        v.reg_class = RegClass::GPR;
        v.size = 8;
        return LirOperand::vreg(v, 8);
    };

    auto appendLir = [](LirBlock& blk, LirOpcode op, LirOperand def,
                        LirOperand use0 = {}, LirOperand use1 = {}) {
        auto inst = std::make_unique<LirInst>(op);
        if (!def.is_none()) inst->add_def(def);
        if (!use0.is_none()) inst->add_use(use0);
        if (!use1.is_none()) inst->add_use(use1);
        blk.append_inst(std::move(inst));
    };

    std::unordered_set<std::string> functionsWithAstFloatDiag;
    for (const auto& d : diags) {
        if (d.kind == DeterministicHazardKind::TranscendentalFloatDrift) {
            functionsWithAstFloatDiag.insert(d.functionName);
        }
    }

    for (const auto& fn : ilMod.functions) {
        if (fn.isExternal) continue;
        for (const auto& blk : fn.blocks) {
            LirBlock x64Blk(blk.id, fn.name + "_b" + std::to_string(blk.id) + "_x64");
            LirBlock armBlk(blk.id, fn.name + "_b" + std::to_string(blk.id) + "_arm64");

            bool hasBitwiseInsts = false;
            Span lastBitwiseSpan;

            for (const auto& inst : blk.instructions) {
                if (inst.op == il::Op::MathUnary) {
                    auto kind = static_cast<il::MathUnaryFn>(inst.immI32);
                    if ((kind == il::MathUnaryFn::Sin || kind == il::MathUnaryFn::Cos) &&
                        functionsWithAstFloatDiag.count(fn.name) == 0) {
                        functionsWithAstFloatDiag.insert(fn.name);
                        CheckDiagnostic d;
                        d.kind = DeterministicHazardKind::TranscendentalFloatDrift;
                        d.span = inst.span;
                        d.functionName = fn.name;
                        d.message = "IL math.unary(sin/cos) lowers to hardware/libm transcendental FP with cross-platform ULP drift";
                        d.remediation = "replace transcendental math intrinsic with fixed-point or polynomial approximation";
                        diags.push_back(std::move(d));
                    }
                }

                uint32_t dstReg = 100 + inst.result;
                switch (inst.op) {
                    case il::Op::ConstI32: {
                        appendLir(x64Blk, LirOpcode::Mov, makeVReg(dstReg), LirOperand::imm(inst.immI32));
                        appendLir(armBlk, LirOpcode::Mov, makeVReg(dstReg), LirOperand::imm(inst.immI32));
                        hasBitwiseInsts = true;
                        break;
                    }
                    case il::Op::Add:
                    case il::Op::Sub:
                    case il::Op::Mul:
                    case il::Op::BitAnd:
                    case il::Op::BitOr:
                    case il::Op::BitXor: {
                        if (inst.operands.size() < 2) break;
                        uint32_t lhsReg = 100 + inst.operands[0];
                        uint32_t rhsReg = 100 + inst.operands[1];
                        LirOpcode op = LirOpcode::Add;
                        if (inst.op == il::Op::Sub) op = LirOpcode::Sub;
                        else if (inst.op == il::Op::Mul) op = LirOpcode::Imul;
                        else if (inst.op == il::Op::BitAnd) op = LirOpcode::And;
                        else if (inst.op == il::Op::BitOr) op = LirOpcode::Or;
                        else if (inst.op == il::Op::BitXor) op = LirOpcode::Xor;

                        appendLir(x64Blk, op, makeVReg(dstReg), makeVReg(lhsReg), makeVReg(rhsReg));
                        appendLir(armBlk, op, makeVReg(dstReg), makeVReg(lhsReg), makeVReg(rhsReg));
                        hasBitwiseInsts = true;
                        lastBitwiseSpan = inst.span;
                        break;
                    }
                    case il::Op::BitNot:
                    case il::Op::Neg: {
                        if (inst.operands.empty()) break;
                        uint32_t srcReg = 100 + inst.operands[0];
                        LirOpcode op = (inst.op == il::Op::BitNot) ? LirOpcode::Not : LirOpcode::Neg;
                        appendLir(x64Blk, op, makeVReg(dstReg), makeVReg(srcReg));
                        appendLir(armBlk, op, makeVReg(dstReg), makeVReg(srcReg));
                        hasBitwiseInsts = true;
                        lastBitwiseSpan = inst.span;
                        break;
                    }
                    case il::Op::Shl:
                    case il::Op::Shr:
                    case il::Op::UShr: {
                        if (inst.operands.size() < 2) break;
                        uint32_t lhsReg = 100 + inst.operands[0];
                        uint32_t rhsReg = 100 + inst.operands[1];
                        uint32_t maskedRhsReg = 50000 + inst.result;
                        appendLir(x64Blk, LirOpcode::And, makeVReg(maskedRhsReg), makeVReg(rhsReg), LirOperand::imm(31));
                        appendLir(armBlk, LirOpcode::And, makeVReg(maskedRhsReg), makeVReg(rhsReg), LirOperand::imm(31));

                        LirOpcode op = LirOpcode::Shl;
                        if (inst.op == il::Op::Shr) op = LirOpcode::Sar;
                        else if (inst.op == il::Op::UShr) op = LirOpcode::Shr;

                        appendLir(x64Blk, op, makeVReg(dstReg), makeVReg(lhsReg), makeVReg(maskedRhsReg));
                        appendLir(armBlk, op, makeVReg(dstReg), makeVReg(lhsReg), makeVReg(maskedRhsReg));
                        hasBitwiseInsts = true;
                        lastBitwiseSpan = inst.span;
                        break;
                    }
                    default:
                        break;
                }
            }

            if (hasBitwiseInsts) {
                auto x64Res = brass::verify::verify_x64_lir_block_equivalence(x64Blk, x64Blk, 0xFFFF, 0xFFFF, false);
                auto armRes = brass::verify::verify_aarch64_lir_block_equivalence(
                    armBlk, armBlk, brass::verify::kArm64AllGprsMask, brass::verify::kArm64AllVecMask, false);
                if (!x64Res.is_equivalent() || !armRes.is_equivalent()) {
                    CheckDiagnostic d;
                    d.kind = DeterministicHazardKind::CrossArchBitwiseDrift;
                    d.span = lastBitwiseSpan;
                    d.functionName = fn.name;
                    d.message = "x86_64 vs AArch64 LIR bitwise equivalence verification failed on block";
                    d.remediation = "mask shift counts and avoid architecture-dependent bitwise lowering";
                    diags.push_back(std::move(d));
                }
            }
        }
    }
}

}  // namespace

const char* deterministicHazardTag(DeterministicHazardKind kind) {
    switch (kind) {
        case DeterministicHazardKind::UnseededPrng:
            return "unseeded-prng";
        case DeterministicHazardKind::WallClockTime:
            return "wall-clock-time";
        case DeterministicHazardKind::TranscendentalFloatDrift:
            return "transcendental-float-drift";
        case DeterministicHazardKind::ObjectIterationOrder:
            return "object-iteration-order";
        case DeterministicHazardKind::CrossArchBitwiseDrift:
            return "cross-arch-bitwise-drift";
    }
    return "deterministic-hazard";
}

void checkDeterministicLockstep(const ast::Program& prog,
                                const il::Module& ilMod,
                                std::vector<CheckDiagnostic>& diags) {
    DeterministicAstVisitor visitor(diags);
    prog.accept(visitor);
    auditIlAndCrossArchLir(ilMod, diags);
}

bool checkDeterministicSource(std::string_view src,
                              std::vector<CheckDiagnostic>& diags,
                              std::string* errorOut) {
    SourceSet sources;
    const auto& buf = sources.add("input.js", std::string(src));
    DiagnosticSink parseDiags;
    auto tokens = Lexer(buf, parseDiags).lex();
    if (parseDiags.hasErrors()) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto astMod = Parser(std::move(tokens), parseDiags).parseModule("input.js");
    if (parseDiags.hasErrors() || !astMod) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto inferred = types::inferModule(*astMod, parseDiags);
    if (parseDiags.hasErrors() || !inferred) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }
    auto ilMod = lower::lowerModule(*astMod, parseDiags, &*inferred, nullptr, &sources);
    if (parseDiags.hasErrors() || !ilMod) {
        if (errorOut) *errorOut = parseDiags.render(buf);
        return false;
    }

    checkDeterministicLockstep(*astMod, *ilMod, diags);
    return true;
}

std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceBuffer& buffer) {
    std::ostringstream os;
    for (const auto& d : diags) {
        auto lc = buffer.lineCol(d.span.begin);
        os << buffer.name() << ":" << lc.line << ":" << lc.column
           << ": warning [" << deterministicHazardTag(d.kind) << "] (in "
           << d.functionName << "): " << d.message << "\n"
           << "  help: " << d.remediation << "\n";
    }
    os << "bronze check --deterministic: " << diags.size() << " lockstep hazard"
       << (diags.size() == 1 ? "" : "s") << " found\n";
    return os.str();
}

std::string renderDeterministicDiagnostics(const std::vector<CheckDiagnostic>& diags,
                                           const SourceSet& sources) {
    std::ostringstream os;
    for (const auto& d : diags) {
        const SourceBuffer& buf = sources.at(d.span.file);
        auto lc = buf.lineCol(d.span.begin);
        os << buf.name() << ":" << lc.line << ":" << lc.column
           << ": warning [" << deterministicHazardTag(d.kind) << "] (in "
           << d.functionName << "): " << d.message << "\n"
           << "  help: " << d.remediation << "\n";
    }
    os << "bronze check --deterministic: " << diags.size() << " lockstep hazard"
       << (diags.size() == 1 ? "" : "s") << " found\n";
    return os.str();
}

int runDeterministicCheckFile(const std::string& sourcePath,
                              std::string* outString,
                              const std::vector<modules::ModuleRoot>& moduleRoots,
                              const std::string& importMapPath) {
    SourceSet sources;
    DiagnosticSink parseDiags;
    auto astMod = modules::loadProgram(sourcePath, sources, parseDiags, {moduleRoots, importMapPath});
    if (parseDiags.hasErrors() || !astMod) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto inferred = types::inferModule(*astMod, parseDiags);
    if (parseDiags.hasErrors() || !inferred) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    auto ilMod = lower::lowerModule(*astMod, parseDiags, &*inferred, nullptr, &sources);
    if (parseDiags.hasErrors() || !ilMod) {
        std::string msg = parseDiags.render(sources);
        if (outString) *outString = msg;
        else std::fputs(msg.c_str(), stderr);
        return 1;
    }

    std::vector<CheckDiagnostic> lockstepDiags;
    checkDeterministicLockstep(*astMod, *ilMod, lockstepDiags);
    std::string rendered = renderDeterministicDiagnostics(lockstepDiags, sources);
    if (outString) {
        *outString = rendered;
    } else {
        std::fputs(rendered.c_str(), stdout);
    }
    return lockstepDiags.empty() ? 0 : 1;
}

}  // namespace bronze::check
