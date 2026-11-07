#include "check/loop_recurrence_synth.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "check/prove_loop_induction.h"

namespace bronze::check {
namespace {

bool formatPolynomialStepExpr(const ast::Expr* expr,
                              std::string_view ivName,
                              std::string& out) {
    if (!expr) return false;
    if (auto* num = dynamic_cast<const ast::NumberLit*>(expr)) {
        if (!std::isfinite(num->value) || std::trunc(num->value) != num->value) return false;
        if (std::fabs(num->value) > 1000000.0) return false;
        out = std::to_string(static_cast<int64_t>(num->value));
        return true;
    }
    if (auto* id = dynamic_cast<const ast::Ident*>(expr)) {
        if (id->name != ivName) return false;
        out = id->name;
        return true;
    }
    if (auto* u = dynamic_cast<const ast::Unary*>(expr)) {
        std::string sub;
        if (!formatPolynomialStepExpr(u->operand.get(), ivName, sub)) return false;
        if (u->op == ast::UnaryOp::Negate) {
            out = "(-" + sub + ")";
            return true;
        }
        if (u->op == ast::UnaryOp::Posate) {
            out = "(+" + sub + ")";
            return true;
        }
        return false;
    }
    if (auto* b = dynamic_cast<const ast::Binary*>(expr)) {
        std::string lhs, rhs;
        if (!formatPolynomialStepExpr(b->lhs.get(), ivName, lhs) ||
            !formatPolynomialStepExpr(b->rhs.get(), ivName, rhs)) {
            return false;
        }
        if (b->op == ast::BinaryOp::Add) {
            out = "(" + lhs + " + " + rhs + ")";
            return true;
        }
        if (b->op == ast::BinaryOp::Sub) {
            out = "(" + lhs + " - " + rhs + ")";
            return true;
        }
        if (b->op == ast::BinaryOp::Mul) {
            out = "(" + lhs + " * " + rhs + ")";
            return true;
        }
        if (b->op == ast::BinaryOp::Exp) {
            out = "(" + lhs + " ** " + rhs + ")";
            return true;
        }
        return false;
    }
    return false;
}

struct SignedTerm {
    bool negative = false;
    std::string body;
};

std::string joinSignedTerms(const std::vector<SignedTerm>& terms) {
    if (terms.empty()) return "0";
    std::string res;
    for (size_t i = 0; i < terms.size(); ++i) {
        if (i == 0) {
            res += terms[i].negative ? ("-" + terms[i].body) : terms[i].body;
        } else {
            res += terms[i].negative ? (" - " + terms[i].body) : (" + " + terms[i].body);
        }
    }
    return "(" + res + ")";
}

}  // namespace

std::optional<int64_t> evalPolynomialStepExpr(const ast::Expr* stepExpr,
                                              std::string_view ivName,
                                              int64_t ivVal) {
    if (!stepExpr) return std::nullopt;
    if (auto* num = dynamic_cast<const ast::NumberLit*>(stepExpr)) {
        if (!std::isfinite(num->value) || std::trunc(num->value) != num->value) {
            return std::nullopt;
        }
        if (std::fabs(num->value) > 1000000.0) return std::nullopt;
        return static_cast<int64_t>(num->value);
    }
    if (auto* id = dynamic_cast<const ast::Ident*>(stepExpr)) {
        if (id->name != ivName) return std::nullopt;
        return ivVal;
    }
    if (auto* u = dynamic_cast<const ast::Unary*>(stepExpr)) {
        auto sub = evalPolynomialStepExpr(u->operand.get(), ivName, ivVal);
        if (!sub) return std::nullopt;
        if (u->op == ast::UnaryOp::Negate) return -(*sub);
        if (u->op == ast::UnaryOp::Posate) return *sub;
        return std::nullopt;
    }
    if (auto* b = dynamic_cast<const ast::Binary*>(stepExpr)) {
        auto lhs = evalPolynomialStepExpr(b->lhs.get(), ivName, ivVal);
        auto rhs = evalPolynomialStepExpr(b->rhs.get(), ivName, ivVal);
        if (!lhs || !rhs) return std::nullopt;
        if (b->op == ast::BinaryOp::Add) return *lhs + *rhs;
        if (b->op == ast::BinaryOp::Sub) return *lhs - *rhs;
        if (b->op == ast::BinaryOp::Mul) return *lhs * *rhs;
        if (b->op == ast::BinaryOp::Exp) {
            if (*rhs < 0 || *rhs > 2) return std::nullopt;
            int64_t acc = 1;
            for (int64_t e = 0; e < *rhs; ++e) acc *= *lhs;
            return acc;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

std::optional<SynthesizedClosedForm> synthesizePolynomialLoopClosedForm(
    int64_t initVal,
    std::string_view ivName,
    std::string_view boundName,
    const ast::Expr* stepExpr,
    bool wrapsInt32) {
    if (ivName.empty() || boundName.empty() || ivName == boundName) {
        return std::nullopt;
    }
    if (initVal < -1000000 || initVal > 1000000) {
        return std::nullopt;
    }

    std::string stepJs;
    if (!formatPolynomialStepExpr(stepExpr, ivName, stepJs)) {
        return std::nullopt;
    }

    int64_t S[7] = {};
    S[0] = initVal;
    for (int k = 1; k <= 6; ++k) {
        auto stepVal = evalPolynomialStepExpr(stepExpr, ivName, k - 1);
        if (!stepVal) return std::nullopt;
        S[k] = S[k - 1] + *stepVal;
    }

    int64_t d0 = S[0];
    int64_t d1 = S[1] - S[0];
    int64_t d2 = S[2] - 2 * S[1] + S[0];
    int64_t d3 = S[3] - 3 * S[2] + 3 * S[1] - S[0];
    int64_t d4 = S[4] - 4 * S[3] + 6 * S[2] - 4 * S[1] + S[0];
    if (d4 != 0) return std::nullopt;

    for (int64_t k = 0; k <= 6; ++k) {
        int64_t bin2 = (k * (k - 1)) / 2;
        int64_t bin3 = (k * (k - 1) * (k - 2)) / 6;
        int64_t pred = d0 + d1 * k + d2 * bin2 + d3 * bin3;
        if (pred != S[k]) return std::nullopt;
    }

    const std::string n(boundName);
    std::string sumExpr;
    int degree = 0;

    if (d0 == 0 && d1 == 0 && d2 == 1 && d3 == 0) {
        // Classic Gauss sum: preserve exact formatting expected by existing tests.
        sumExpr = "((" + n + " * (" + n + " - 1)) / 2)";
        degree = 2;
    } else if (d0 == 0 && d1 == 1 && d2 == 2 && d3 == 0) {
        // Sum of first n odd numbers: sum_{i=0}^{n-1} (2i + 1) = n^2.
        sumExpr = "(" + n + " * " + n + ")";
        degree = 2;
    } else if (d0 == 0 && d1 == 0 && d2 == 1 && d3 == 2) {
        // Sum of squares: sum_{i=0}^{n-1} i^2 = n(n - 1)(2n - 1) / 6.
        sumExpr = "((" + n + " * (" + n + " - 1) * (2 * " + n + " - 1)) / 6)";
        degree = 3;
    } else if (d3 == 0) {
        degree = (d2 != 0) ? 2 : (d1 != 0 ? 1 : 0);
        std::vector<SignedTerm> terms;
        if (d2 != 0) {
            bool neg = d2 < 0;
            int64_t ad2 = neg ? -d2 : d2;
            if (ad2 % 2 == 0) {
                int64_t c2 = ad2 / 2;
                std::string body = (c2 == 1)
                                       ? (n + " * (" + n + " - 1)")
                                       : (std::to_string(c2) + " * " + n + " * (" + n + " - 1)");
                terms.push_back({neg, body});
            } else {
                std::string body = (ad2 == 1)
                                       ? ("((" + n + " * (" + n + " - 1)) / 2)")
                                       : ("((" + std::to_string(ad2) + " * " + n + " * (" + n + " - 1)) / 2)");
                terms.push_back({neg, body});
            }
        }
        if (d1 != 0) {
            bool neg = d1 < 0;
            int64_t ad1 = neg ? -d1 : d1;
            std::string body = (ad1 == 1) ? n : (std::to_string(ad1) + " * " + n);
            terms.push_back({neg, body});
        }
        if (d0 != 0 || terms.empty()) {
            bool neg = d0 < 0;
            int64_t ad0 = neg ? -d0 : d0;
            terms.push_back({neg, std::to_string(ad0)});
        }
        sumExpr = joinSignedTerms(terms);
    } else {
        degree = 3;
        std::vector<SignedTerm> numTerms;
        {
            bool neg3 = d3 < 0;
            int64_t ad3 = neg3 ? -d3 : d3;
            std::string body3 = (ad3 == 1)
                                    ? (n + " * (" + n + " - 1) * (" + n + " - 2)")
                                    : (std::to_string(ad3) + " * " + n + " * (" + n + " - 1) * (" + n + " - 2)");
            numTerms.push_back({neg3, body3});
        }
        if (d2 != 0) {
            int64_t c2 = 3 * d2;
            bool neg2 = c2 < 0;
            int64_t ac2 = neg2 ? -c2 : c2;
            std::string body2 = std::to_string(ac2) + " * " + n + " * (" + n + " - 1)";
            numTerms.push_back({neg2, body2});
        }
        if (d1 != 0) {
            int64_t c1 = 6 * d1;
            bool neg1 = c1 < 0;
            int64_t ac1 = neg1 ? -c1 : c1;
            std::string body1 = std::to_string(ac1) + " * " + n;
            numTerms.push_back({neg1, body1});
        }
        std::string fracPart = "(" + joinSignedTerms(numTerms) + " / 6)";
        if (d0 == 0) {
            sumExpr = fracPart;
        } else if (d0 > 0) {
            sumExpr = "(" + fracPart + " + " + std::to_string(d0) + ")";
        } else {
            sumExpr = "(" + fracPart + " - " + std::to_string(-d0) + ")";
        }
    }

    std::string initStr = wrapsInt32
                              ? std::to_string(static_cast<int32_t>(initVal))
                              : std::to_string(initVal);
    std::string wrappedSum = wrapsInt32 ? ("(" + sumExpr + " | 0)") : sumExpr;
    std::string retStmt = "return (" + n + " <= 0) ? " + initStr + " : " + wrappedSum + ";";

    // Build verification functions and prove equivalence via k-induction.
    std::string iv(ivName);
    std::string loopBodyStmt = wrapsInt32
                                   ? ("s = (s + " + stepJs + ") | 0;")
                                   : ("s += " + stepJs + ";");
    std::string loopFnSrc =
        "function __synth_check(" + n + ") {\n"
        "    let s = " + initStr + ";\n"
        "    for (let " + iv + " = 0; " + iv + " < " + n + "; " + iv + "++) {\n"
        "        " + loopBodyStmt + "\n"
        "    }\n"
        "    return s;\n"
        "}\n";
    std::string closedFnSrc =
        "function __synth_check(" + n + ") {\n"
        "    " + retStmt + "\n"
        "}\n";

    int32_t verifyMax = (degree >= 3) ? 500 : 10000;
    EquivReport rep = proveLoopClosedFormInductive(loopFnSrc, closedFnSrc, 0, verifyMax);
    if (!rep.isEquivalent()) {
        return std::nullopt;
    }

    SynthesizedClosedForm result;
    result.expression = std::move(sumExpr);
    result.returnStatement = std::move(retStmt);
    result.initVal = initVal;
    result.d0 = d0;
    result.d1 = d1;
    result.d2 = d2;
    result.d3 = d3;
    result.degree = degree;
    return result;
}

}  // namespace bronze::check
