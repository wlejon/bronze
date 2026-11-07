#include "check/js_subexpr_superopt.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/assigned.h"
#include "ast/query_walk.h"

namespace bronze::check::jsrw {

using namespace bronze::ast;

namespace {

bool isBitwiseBinaryOp(BinaryOp op) {
    switch (op) {
        case BinaryOp::BitAnd:
        case BinaryOp::BitOr:
        case BinaryOp::BitXor:
        case BinaryOp::Shl:
        case BinaryOp::Shr:
        case BinaryOp::UShr:
            return true;
        default:
            return false;
    }
}

bool isSignedInt32BitwiseRoot(const Expr& e) {
    if (auto* u = exactAs<const Unary*>(&e)) {
        return u->op == UnaryOp::BitNot;
    }
    if (auto* b = exactAs<const Binary*>(&e)) {
        switch (b->op) {
            case BinaryOp::BitAnd:
            case BinaryOp::BitOr:
            case BinaryOp::BitXor:
            case BinaryOp::Shl:
            case BinaryOp::Shr:
                return true;
            default:
                return false;
        }
    }
    return false;
}

bool isForbiddenIdentName(const std::string& name) {
    return name.empty() || name == "eval" || name == "arguments" || name == "Math" ||
           name == "Array" || name == "Object" || name == "Number" || name == "String" ||
           name == "Boolean" || name == "BigInt" || name == "Symbol" || name == "NaN" ||
           name == "Infinity" || name == "undefined" || name == "globalThis" ||
           name.find('.') != std::string::npos;
}

struct SubexprAnalysis {
    uint32_t astCost = 0;
    uint32_t opCount = 0;
    std::vector<std::string> varOrder;
    std::unordered_map<std::string, std::vector<const Ident*>> varOccurrences;
    std::unordered_map<std::string, std::string> synthToSource;
    std::unordered_map<std::string, std::string> sourceToSynth;
    std::vector<std::pair<Extent, std::string>> memberSpanRewrites;
    std::unordered_set<std::string> uncoercedVars;
};

bool analyzeSubexprNode(const ParsedJs& p,
                        const std::unordered_set<std::string>& rebound,
                        const Expr* e,
                        bool parentCoercesToInt32,
                        bool allowRelational,
                        SubexprAnalysis& info) {
    if (!e) return false;

    if (auto* n = exactAs<const NumberLit*>(e)) {
        if (!std::isfinite(n->value) || n->value != std::floor(n->value)) return false;
        if (n->value < -2147483648.0 || n->value > 2147483647.0) return false;
        info.astCost += 1;
        return true;
    }

    if (auto* id = exactAs<const Ident*>(e)) {
        if (isForbiddenIdentName(id->name) || rebound.count(id->name) > 0) return false;
        if (info.varOccurrences.find(id->name) == info.varOccurrences.end() &&
            info.synthToSource.find(id->name) == info.synthToSource.end()) {
            info.varOrder.push_back(id->name);
        }
        info.varOccurrences[id->name].push_back(id);
        if (!parentCoercesToInt32) {
            info.uncoercedVars.insert(id->name);
        }
        info.astCost += 1;
        return true;
    }

    if (auto* mem = exactAs<const MemberAccess*>(e)) {
        if (mem->optional || mem->isPrivate || mem->property.empty()) return false;
        std::string baseName;
        if (exactAs<const ThisExpr*>(mem->object.get())) {
            baseName = "this";
        } else if (auto* baseId = exactAs<const Ident*>(mem->object.get())) {
            if (isForbiddenIdentName(baseId->name) || rebound.count(baseId->name) > 0) return false;
            baseName = baseId->name;
        } else {
            return false;
        }
        std::string srcExpr = baseName + "." + mem->property;
        std::string synthName;
        auto it = info.sourceToSynth.find(srcExpr);
        if (it == info.sourceToSynth.end()) {
            synthName = "__f" + std::to_string(info.sourceToSynth.size());
            info.sourceToSynth[srcExpr] = synthName;
            info.synthToSource[synthName] = srcExpr;
            info.varOrder.push_back(synthName);
        } else {
            synthName = it->second;
        }
        info.memberSpanRewrites.push_back({p.inner(*mem), synthName});
        if (!parentCoercesToInt32) {
            info.uncoercedVars.insert(synthName);
        }
        info.astCost += 1;
        return true;
    }

    if (auto* u = exactAs<const Unary*>(e)) {
        if (u->op == UnaryOp::Negate) {
            if (auto* lit = exactAs<const NumberLit*>(u->operand.get())) {
                double nv = -lit->value;
                if (!std::isfinite(nv) || nv != std::floor(nv)) return false;
                if (nv < -2147483648.0 || nv > 2147483647.0) return false;
                info.astCost += 1;
                return true;
            }
            info.astCost += 1;
            info.opCount += 1;
            return analyzeSubexprNode(p, rebound, u->operand.get(), false, false, info);
        }
        if (u->op == UnaryOp::BitNot) {
            info.astCost += 1;
            info.opCount += 1;
            return analyzeSubexprNode(p, rebound, u->operand.get(), true, false, info);
        }
        return false;
    }

    if (auto* b = exactAs<const Binary*>(e)) {
        if (isBitwiseBinaryOp(b->op)) {
            info.astCost += 1;
            info.opCount += 1;
            return analyzeSubexprNode(p, rebound, b->lhs.get(), true, false, info) &&
                   analyzeSubexprNode(p, rebound, b->rhs.get(), true, false, info);
        }
        if (b->op == BinaryOp::Add || b->op == BinaryOp::Sub || b->op == BinaryOp::Mul) {
            bool safeInt32Arith = parentCoercesToInt32;
            if (!safeInt32Arith) {
                auto r = p.rangeOf(*b);
                if (r && r->isInt32()) safeInt32Arith = true;
            }
            if (!safeInt32Arith) return false;
            info.astCost += (b->op == BinaryOp::Mul) ? 2 : 1;
            info.opCount += 1;
            return analyzeSubexprNode(p, rebound, b->lhs.get(), false, false, info) &&
                   analyzeSubexprNode(p, rebound, b->rhs.get(), false, false, info);
        }
        if (allowRelational &&
            (b->op == BinaryOp::Less || b->op == BinaryOp::LessEqual ||
             b->op == BinaryOp::Greater || b->op == BinaryOp::GreaterEqual ||
             b->op == BinaryOp::Eq || b->op == BinaryOp::StrictEq ||
             b->op == BinaryOp::Ne || b->op == BinaryOp::StrictNe)) {
            info.astCost += 1;
            info.opCount += 1;
            return analyzeSubexprNode(p, rebound, b->lhs.get(), false, false, info) &&
                   analyzeSubexprNode(p, rebound, b->rhs.get(), false, false, info);
        }
        return false;
    }

    if (auto* t = exactAs<const Ternary*>(e)) {
        info.astCost += 2;
        info.opCount += 2;
        return analyzeSubexprNode(p, rebound, t->condition.get(), false, true, info) &&
               analyzeSubexprNode(p, rebound, t->thenExpr.get(), false, false, info) &&
               analyzeSubexprNode(p, rebound, t->elseExpr.get(), false, false, info);
    }

    return false;
}

bool hasMatchingOuterParens(std::string_view s) {
    if (s.size() < 2 || s.front() != '(' || s.back() != ')') return false;
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '(') {
            ++depth;
        } else if (s[i] == ')') {
            --depth;
            if (depth == 0 && i + 1 < s.size()) return false;
        }
    }
    return depth == 0;
}

uint32_t countSynthOps(std::string_view s) {
    uint32_t ops = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s.substr(i, 3) == ">>>") {
            ++ops;
            i += 2;
        } else if (s.substr(i, 2) == "<<" || s.substr(i, 2) == ">>") {
            ++ops;
            ++i;
        } else if (s[i] == '&' || s[i] == '|' || s[i] == '^' || s[i] == '~' || s[i] == '+' ||
                   s[i] == '*' || s[i] == '?') {
            ++ops;
        } else if (s[i] == '-') {
            // Skip negative number literal sign
            if (i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') continue;
            ++ops;
        }
    }
    return ops;
}

bool hasBitwiseOpInText(std::string_view s) {
    for (char c : s) {
        if (c == '&' || c == '|' || c == '^' || c == '~' || c == '<' || c == '>') return true;
    }
    return false;
}

Edit makeSubexprEdit(const ParsedJs& p,
                     Extent at,
                     std::string text,
                     const Node& site,
                     const std::string& fallbackFn,
                     std::string description) {
    Edit e;
    e.at = at;
    e.text = p.asiSafe(at, std::move(text));
    std::string before = p.slice(at);
    std::string fn = p.functionNameOf(site);
    e.hit = {fn.empty() ? fallbackFn : fn,
             "subexpr-superopt",
             std::move(description),
             before,
             e.text,
             true,
             fnv1aHex(before + "->" + e.text)};
    return e;
}

class SubexprSuperoptWalker final : public detail::IdentVisitor {
public:
    using detail::IdentVisitor::visit;

    SubexprSuperoptWalker(const ParsedJs& p,
                          const std::string& fallbackFn,
                          const std::unordered_set<std::string>& rebound,
                          std::vector<Edit>& out,
                          int32_t domainMin,
                          int32_t domainMax,
                          CegisSuperoptStats* accumStats)
        : p_(p),
          fallbackFn_(fallbackFn),
          rebound_(rebound),
          out_(out),
          domainMin_(domainMin),
          domainMax_(domainMax),
          accumStats_(accumStats) {}

    void visit(const FunctionDecl& d) override {
        auto saved = rebound_;
        rebound_ = getDeeplyReboundNames(d.body);
        detail::IdentVisitor::visit(d);
        rebound_ = std::move(saved);
    }

    void visit(const FunctionExpr& f) override {
        auto saved = rebound_;
        rebound_ = getDeeplyReboundNames(f.body);
        detail::IdentVisitor::visit(f);
        rebound_ = std::move(saved);
    }

    void visit(const Unary& u) override {
        if (trySuperoptExpr(u)) return;
        detail::IdentVisitor::visit(u);
    }

    void visit(const Binary& b) override {
        if (trySuperoptExpr(b)) return;
        detail::IdentVisitor::visit(b);
    }

    void visit(const Ternary& t) override {
        if (trySuperoptExpr(t)) return;
        detail::IdentVisitor::visit(t);
    }

private:
    const ParsedJs& p_;
    const std::string& fallbackFn_;
    std::unordered_set<std::string> rebound_;
    std::vector<Edit>& out_;
    int32_t domainMin_;
    int32_t domainMax_;
    CegisSuperoptStats* accumStats_;

    bool trySuperoptExpr(const Expr& e) {
        const bool isBitwiseRoot = isSignedInt32BitwiseRoot(e);
        const bool isTernaryRoot = exactAs<const Ternary*>(&e) != nullptr;
        if (!isBitwiseRoot && !isTernaryRoot) return false;

        SubexprAnalysis info;
        if (!analyzeSubexprNode(p_, rebound_, &e, isBitwiseRoot, false, info)) {
            return false;
        }
        if (info.astCost < 3 || info.opCount < 2) return false;
        if (info.varOrder.empty() || info.varOrder.size() > 3) return false;

        std::unordered_map<std::string, bool> varKnownInt32;
        bool allVarsHaveRange = true;
        int32_t tightMin = 2147483647;
        int32_t tightMax = -2147483647 - 1;

        for (const std::string& v : info.varOrder) {
            auto occIt = info.varOccurrences.find(v);
            bool knownInt32 = (occIt != info.varOccurrences.end() && !occIt->second.empty());
            if (!knownInt32) {
                allVarsHaveRange = false;
            } else {
                for (const Ident* id : occIt->second) {
                    auto r = p_.rangeOf(*id);
                    if (!r || !r->isInt32()) {
                        knownInt32 = false;
                        allVarsHaveRange = false;
                    } else {
                        int32_t rLo = static_cast<int32_t>(std::max(-2147483648.0, std::floor(r->lo)));
                        int32_t rHi = static_cast<int32_t>(std::min(2147483647.0, std::ceil(r->hi)));
                        tightMin = std::min(tightMin, rLo);
                        tightMax = std::max(tightMax, rHi);
                    }
                }
            }
            varKnownInt32[v] = knownInt32;
            if (!knownInt32) {
                if (isTernaryRoot || info.uncoercedVars.count(v) > 0) {
                    return false;
                }
            }
        }

        const Extent rootExt = p_.inner(e);
        const std::string origText = p_.slice(rootExt);
        std::string synthInputText = origText;
        if (!info.memberSpanRewrites.empty()) {
            auto spans = info.memberSpanRewrites;
            std::sort(spans.begin(), spans.end(),
                      [](const auto& a, const auto& b) { return a.first.begin > b.first.begin; });
            for (const auto& [ext, sname] : spans) {
                if (ext.begin >= rootExt.begin && ext.end <= rootExt.end && ext.end >= ext.begin) {
                    synthInputText.replace(ext.begin - rootExt.begin, ext.end - ext.begin, sname);
                }
            }
        }
        std::ostringstream fnOss;
        fnOss << "function __sub(";
        for (size_t i = 0; i < info.varOrder.size(); ++i) {
            if (i > 0) fnOss << ", ";
            fnOss << info.varOrder[i];
        }
        fnOss << ") { return (" << synthInputText << "); }";

        CegisSuperoptOptions sopts;
        sopts.domainMin = domainMin_;
        sopts.domainMax = domainMax_;
        if (allVarsHaveRange && tightMin <= tightMax) {
            sopts.domainMin = std::clamp(tightMin, domainMin_, domainMax_);
            sopts.domainMax = std::clamp(tightMax, domainMin_, domainMax_);
        }
        sopts.maxCandidates = 256;
        sopts.maxAllowedCost = info.astCost - 1;
        sopts.maxEnumCost = std::min<uint32_t>(3u, info.astCost - 1);
        sopts.maxEnumNodes = 512;
        sopts.maxSmtQueries = 8;
        sopts.requireLowerCost = true;

        CegisSuperoptResult sres = superoptimizeJsFunction(fnOss.str(), sopts);
        if (accumStats_) {
            accumStats_->candidatesGenerated += sres.stats.candidatesGenerated;
            accumStats_->candidatesTested += sres.stats.candidatesTested;
            accumStats_->concretePruned += sres.stats.concretePruned;
            accumStats_->smtQueries += sres.stats.smtQueries;
            accumStats_->counterexamplesFound += sres.stats.counterexamplesFound;
        }

        if (!sres.verified || !sres.optimized || sres.synthesizedExpression.empty()) {
            return false;
        }

        std::string synth = sres.synthesizedExpression;
        uint32_t costAfter = sres.stats.costAfter;

        // If `synth` is a bare variable that is not statically proven to be int32,
        // coerce it via `| 0` so non-int32 inputs preserve exact ToInt32 semantics.
        for (const std::string& v : info.varOrder) {
            if (synth == v && !varKnownInt32[v]) {
                synth = "(" + v + " | 0)";
                costAfter = std::max<uint32_t>(costAfter, 3u);
            }
        }
        if (!hasBitwiseOpInText(synth)) {
            for (const std::string& v : info.varOrder) {
                if (synth == v && !varKnownInt32[v]) {
                    return false;
                }
            }
        }

        if (costAfter >= sres.stats.costBefore || costAfter >= info.astCost) {
            return false;
        }

        for (const auto& [sname, srcExpr] : info.synthToSource) {
            size_t pos = 0;
            while ((pos = synth.find(sname, pos)) != std::string::npos) {
                synth.replace(pos, sname.size(), srcExpr);
                pos += srcExpr.size();
            }
        }

        uint32_t synthOps = countSynthOps(synth);
        std::string replacement = hasMatchingOuterParens(synth) ? synth : "(" + synth + ")";
        if (synthOps >= info.opCount && replacement.size() >= origText.size()) {
            return false;
        }

        std::string desc = "CEGIS sub-expression superoptimization (" + sres.ruleDescription +
                           "): `" + origText + "` -> `" + replacement + "`";
        out_.push_back(makeSubexprEdit(p_, p_.inner(e), std::move(replacement), e, fallbackFn_,
                                       std::move(desc)));
        return true;
    }
};

}  // namespace

void collectSubexprSuperoptEdits(const ParsedJs& p,
                                 const std::string& fallbackFn,
                                 std::vector<Edit>& out,
                                 int32_t domainMin,
                                 int32_t domainMax,
                                 CegisSuperoptStats* accumStats) {
    std::unordered_set<std::string> rebound = getDeeplyReboundNames(p.module().body);
    SubexprSuperoptWalker walker(p, fallbackFn, rebound, out, domainMin, domainMax, accumStats);
    p.module().accept(walker);
}

}  // namespace bronze::check::jsrw
