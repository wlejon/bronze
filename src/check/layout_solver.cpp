#include "check/layout_solver.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include <verify/bv_solver.hpp>

#include "check/prove_equiv.h"

namespace bronze::check {
namespace {

using brass::verify::BVContext;
using brass::verify::BVNodeId;
using brass::verify::BVOp;
using brass::verify::SolveOutcome;

struct BoxNodes {
    std::string id;
    bool is3D = false;
    uint32_t idxX = 0;
    uint32_t idxY = 0;
    uint32_t idxZ = 0;
    uint32_t idxW = 0;
    uint32_t idxH = 0;
    uint32_t idxD = 0;
    BVNodeId x = 0;
    BVNodeId y = 0;
    BVNodeId z = 0;
    BVNodeId w = 0;
    BVNodeId h = 0;
    BVNodeId d = 0;
};

BVNodeId makeSnapConstraint(BVContext& ctx, BVNodeId val64, int32_t grid) {
    if (grid <= 1) return ctx.make_const(1, 1);
    BVNodeId zero64 = ctx.make_const(0, 64);
    uint64_t ugrid = static_cast<uint64_t>(grid);
    if ((ugrid & (ugrid - 1)) == 0) {
        BVNodeId mask = ctx.make_const(ugrid - 1, 64);
        return ctx.make_binary(BVOp::Eq, ctx.make_binary(BVOp::And, val64, mask), zero64);
    }
    BVNodeId v32 = ctx.make_extract(val64, 31, 0);
    BVNodeId g32 = ctx.make_const(ugrid, 32);
    BVNodeId rem32 = brass::verify::expand_const_div_rem_bv(ctx, BVOp::SRem, v32, g32, 32);
    return ctx.make_binary(BVOp::Eq, ctx.make_sext(rem32, 32), zero64);
}

// Recursive-descent parser for layout constraint expressions like:
//   "header.y == 0 && main.x >= sidebar.x + sidebar.w + 8"
//   "panel.w * 9 == panel.h * 16"
//   "a.x != b.x || a.y != b.y"
class ConstraintParser {
public:
    ConstraintParser(std::string_view src,
                     BVContext& ctx,
                     const std::unordered_map<std::string, BoxNodes>& boxMap,
                     const LayoutSpec& spec)
        : src_(src), ctx_(ctx), boxMap_(boxMap), spec_(spec) {}

    bool parse(BVNodeId& outBool) {
        BVNodeId expr = parseOr();
        skipWs();
        if (!ok_ || pos_ < src_.size()) return false;
        outBool = toBool1(expr);
        return true;
    }

private:
    std::string_view src_;
    size_t pos_ = 0;
    bool ok_ = true;
    BVContext& ctx_;
    const std::unordered_map<std::string, BoxNodes>& boxMap_;
    const LayoutSpec& spec_;

    void skipWs() {
        while (pos_ < src_.size() && std::isspace(static_cast<unsigned char>(src_[pos_]))) {
            ++pos_;
        }
    }

    bool match(std::string_view tok) {
        skipWs();
        if (src_.substr(pos_).rfind(tok, 0) == 0) {
            pos_ += tok.size();
            return true;
        }
        return false;
    }

    BVNodeId toBool1(BVNodeId n) {
        if (ctx_.width(n) == 1) return n;
        return ctx_.make_binary(BVOp::Ne, toBV64(n), ctx_.make_const(0, 64));
    }

    BVNodeId toBV64(BVNodeId n) {
        uint32_t w = ctx_.width(n);
        if (w >= 64) return n;
        if (w == 1) return ctx_.make_zext(n, 63);
        return ctx_.make_sext(n, 64 - w);
    }

    BVNodeId parseOr() {
        BVNodeId left = parseAnd();
        while (ok_) {
            skipWs();
            if (src_.substr(pos_).rfind("||", 0) == 0) {
                pos_ += 2;
                BVNodeId right = parseAnd();
                left = ctx_.make_binary(BVOp::Or, toBool1(left), toBool1(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseAnd() {
        BVNodeId left = parseComparison();
        while (ok_) {
            skipWs();
            if (src_.substr(pos_).rfind("&&", 0) == 0) {
                pos_ += 2;
                BVNodeId right = parseComparison();
                left = ctx_.make_binary(BVOp::And, toBool1(left), toBool1(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseComparison() {
        BVNodeId left = parseBitOr();
        skipWs();
        if (match("===") || match("==")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Eq, toBV64(left), toBV64(right));
        }
        if (match("!==") || match("!=")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Ne, toBV64(left), toBV64(right));
        }
        if (match("<=")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Sle, toBV64(left), toBV64(right));
        }
        if (match(">=")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Sge, toBV64(left), toBV64(right));
        }
        if (match("<")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Slt, toBV64(left), toBV64(right));
        }
        if (match(">")) {
            BVNodeId right = parseBitOr();
            return ctx_.make_binary(BVOp::Sgt, toBV64(left), toBV64(right));
        }
        return left;
    }

    BVNodeId parseBitOr() {
        BVNodeId left = parseBitXor();
        while (ok_) {
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '|' &&
                (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '|')) {
                ++pos_;
                BVNodeId right = parseBitXor();
                left = ctx_.make_binary(BVOp::Or, toBV64(left), toBV64(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseBitXor() {
        BVNodeId left = parseBitAnd();
        while (ok_ && match("^")) {
            BVNodeId right = parseBitAnd();
            left = ctx_.make_binary(BVOp::Xor, toBV64(left), toBV64(right));
        }
        return left;
    }

    BVNodeId parseBitAnd() {
        BVNodeId left = parseShift();
        while (ok_) {
            skipWs();
            if (pos_ < src_.size() && src_[pos_] == '&' &&
                (pos_ + 1 >= src_.size() || src_[pos_ + 1] != '&')) {
                ++pos_;
                BVNodeId right = parseShift();
                left = ctx_.make_binary(BVOp::And, toBV64(left), toBV64(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseShift() {
        BVNodeId left = parseAddSub();
        while (ok_) {
            if (match("<<")) {
                BVNodeId right = parseAddSub();
                left = ctx_.make_binary(BVOp::Shl, toBV64(left), toBV64(right));
            } else if (match(">>")) {
                BVNodeId right = parseAddSub();
                left = ctx_.make_binary(BVOp::AShr, toBV64(left), toBV64(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseAddSub() {
        BVNodeId left = parseMulDiv();
        while (ok_) {
            if (match("+")) {
                BVNodeId right = parseMulDiv();
                left = ctx_.make_binary(BVOp::Add, toBV64(left), toBV64(right));
            } else if (match("-")) {
                BVNodeId right = parseMulDiv();
                left = ctx_.make_binary(BVOp::Sub, toBV64(left), toBV64(right));
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseMulDiv() {
        BVNodeId left = parseUnary();
        while (ok_) {
            if (match("*")) {
                BVNodeId right = parseUnary();
                left = ctx_.make_binary(BVOp::Mul, toBV64(left), toBV64(right));
            } else if (match("/")) {
                BVNodeId right = parseUnary();
                left = ctx_.make_binary(BVOp::SDiv, toBV64(left), toBV64(right));
            } else if (match("%")) {
                BVNodeId right = parseUnary();
                BVNodeId r64 = toBV64(right);
                const auto& rNode = ctx_.nodes()[r64];
                if (rNode.op == BVOp::Const && static_cast<int64_t>(rNode.imm) > 1) {
                    int64_t div = static_cast<int64_t>(rNode.imm);
                    uint64_t udiv = static_cast<uint64_t>(div);
                    if ((udiv & (udiv - 1)) == 0) {
                        left = ctx_.make_binary(BVOp::And, toBV64(left), ctx_.make_const(udiv - 1, 64));
                    } else {
                        BVNodeId l32 = ctx_.make_extract(toBV64(left), 31, 0);
                        BVNodeId d32 = ctx_.make_const(udiv, 32);
                        BVNodeId rem32 = brass::verify::expand_const_div_rem_bv(ctx_, BVOp::SRem, l32, d32, 32);
                        left = ctx_.make_sext(rem32, 32);
                    }
                } else {
                    left = ctx_.make_binary(BVOp::SRem, toBV64(left), r64);
                }
            } else {
                break;
            }
        }
        return left;
    }

    BVNodeId parseUnary() {
        skipWs();
        if (match("-")) {
            BVNodeId v = parseUnary();
            return ctx_.make_unary(BVOp::Neg, toBV64(v));
        }
        if (match("!")) {
            BVNodeId v = parseUnary();
            return ctx_.make_unary(BVOp::Not, toBool1(v));
        }
        return parsePrimary();
    }

    BVNodeId parsePrimary() {
        skipWs();
        if (match("(")) {
            BVNodeId inner = parseOr();
            if (!match(")")) ok_ = false;
            return inner;
        }
        if (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
            int64_t val = 0;
            while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) {
                val = val * 10 + (src_[pos_] - '0');
                ++pos_;
            }
            return ctx_.make_const(static_cast<uint64_t>(val), 64);
        }
        // Identifier or `<boxId>.<prop>` or `<boxId>_<prop>`
        size_t start = pos_;
        while (pos_ < src_.size() &&
               (std::isalnum(static_cast<unsigned char>(src_[pos_])) ||
                src_[pos_] == '_' || src_[pos_] == '.' || src_[pos_] == '$')) {
            ++pos_;
        }
        if (pos_ == start) {
            ok_ = false;
            return ctx_.make_const(0, 64);
        }
        std::string token(src_.substr(start, pos_ - start));
        return resolveSymbol(token);
    }

    BVNodeId resolveSymbol(const std::string& sym) {
        if (sym == "container.w" || sym == "container.width") {
            return ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec_.containerW)), 64);
        }
        if (sym == "container.h" || sym == "container.height") {
            return ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec_.containerH)), 64);
        }
        if (sym == "container.d" || sym == "container.depth") {
            return ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec_.containerD)), 64);
        }
        if (sym == "gap") {
            return ctx_.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec_.gap)), 64);
        }

        size_t sep = sym.find('.');
        if (sep == std::string::npos) sep = sym.rfind('_');
        if (sep != std::string::npos) {
            std::string boxId = sym.substr(0, sep);
            std::string prop = sym.substr(sep + 1);
            auto it = boxMap_.find(boxId);
            if (it != boxMap_.end()) {
                const BoxNodes& b = it->second;
                if (prop == "x" || prop == "left") return b.x;
                if (prop == "y" || prop == "top") return b.y;
                if (prop == "z") return b.z;
                if (prop == "w" || prop == "width") return b.w;
                if (prop == "h" || prop == "height") return b.h;
                if (prop == "d" || prop == "depth") return b.d;
                if (prop == "right") return ctx_.make_binary(BVOp::Add, b.x, b.w);
                if (prop == "bottom") return ctx_.make_binary(BVOp::Add, b.y, b.h);
                if (prop == "centerX") {
                    return ctx_.make_binary(BVOp::Add, b.x,
                                            ctx_.make_binary(BVOp::AShr, b.w, ctx_.make_const(1, 64)));
                }
                if (prop == "centerY") {
                    return ctx_.make_binary(BVOp::Add, b.y,
                                            ctx_.make_binary(BVOp::AShr, b.h, ctx_.make_const(1, 64)));
                }
            }
        }
        ok_ = false;
        return ctx_.make_const(0, 64);
    }
};

}  // namespace

LayoutSolution solveLayoutSpec(const LayoutSpec& spec) {
    auto t0 = std::chrono::steady_clock::now();
    LayoutSolution sol;

    BVContext ctx;
    BVNodeId cond = ctx.make_const(1, 1);
    BVNodeId zero64 = ctx.make_const(0, 64);

    std::vector<BoxNodes> boxNodes;
    std::unordered_map<std::string, BoxNodes> boxMap;
    std::vector<BVNodeId> allVarNodes;
    uint32_t nextVarIdx = 0;

    auto allocVar = [&](const std::string& name, uint32_t& outIdx) -> BVNodeId {
        outIdx = nextVarIdx++;
        BVNodeId raw32 = ctx.make_var(name, 32);
        BVNodeId s64 = ctx.make_sext(raw32, 32);
        allVarNodes.push_back(s64);
        return s64;
    };

    auto addBound = [&](BVNodeId node, int32_t lo, int32_t hi) {
        BVNodeId loNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(lo)), 64);
        BVNodeId hiNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(hi)), 64);
        cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sge, node, loNode));
        cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Sle, node, hiNode));
    };

    for (const auto& bspec : spec.boxes) {
        BoxNodes bn;
        bn.id = bspec.id;
        bn.is3D = bspec.is3D;
        bn.x = allocVar(bspec.id + "_x", bn.idxX);
        bn.y = allocVar(bspec.id + "_y", bn.idxY);
        bn.w = allocVar(bspec.id + "_w", bn.idxW);
        bn.h = allocVar(bspec.id + "_h", bn.idxH);
        if (bspec.is3D) {
            bn.z = allocVar(bspec.id + "_z", bn.idxZ);
            bn.d = allocVar(bspec.id + "_d", bn.idxD);
        } else {
            bn.z = zero64;
            bn.d = zero64;
        }

        int32_t maxX = spec.hasContainer ? std::min(bspec.maxX, spec.containerW) : bspec.maxX;
        int32_t maxY = spec.hasContainer ? std::min(bspec.maxY, spec.containerH) : bspec.maxY;
        int32_t maxW = spec.hasContainer ? std::min(bspec.maxW, spec.containerW) : bspec.maxW;
        int32_t maxH = spec.hasContainer ? std::min(bspec.maxH, spec.containerH) : bspec.maxH;

        addBound(bn.x, bspec.minX, maxX);
        addBound(bn.y, bspec.minY, maxY);
        addBound(bn.w, bspec.minW, maxW);
        addBound(bn.h, bspec.minH, maxH);

        if (spec.hasContainer) {
            BVNodeId cw = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec.containerW)), 64);
            BVNodeId ch = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec.containerH)), 64);
            cond = ctx.make_binary(
                BVOp::And, cond,
                ctx.make_binary(BVOp::Sle, ctx.make_binary(BVOp::Add, bn.x, bn.w), cw));
            cond = ctx.make_binary(
                BVOp::And, cond,
                ctx.make_binary(BVOp::Sle, ctx.make_binary(BVOp::Add, bn.y, bn.h), ch));
        }

        if (bspec.is3D) {
            int32_t maxZ = spec.hasContainer ? std::min(bspec.maxZ, spec.containerD) : bspec.maxZ;
            int32_t maxD = spec.hasContainer ? std::min(bspec.maxD, spec.containerD) : bspec.maxD;
            addBound(bn.z, bspec.minZ, maxZ);
            addBound(bn.d, bspec.minD, maxD);
            if (spec.hasContainer) {
                BVNodeId cd = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec.containerD)), 64);
                cond = ctx.make_binary(
                    BVOp::And, cond,
                    ctx.make_binary(BVOp::Sle, ctx.make_binary(BVOp::Add, bn.z, bn.d), cd));
            }
        }

        int32_t snap = (bspec.snapGrid > 0) ? bspec.snapGrid : spec.globalSnapGrid;
        if (snap > 1) {
            cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.x, snap));
            cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.y, snap));
            cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.w, snap));
            cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.h, snap));
            if (bspec.is3D) {
                cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.z, snap));
                cond = ctx.make_binary(BVOp::And, cond, makeSnapConstraint(ctx, bn.d, snap));
            }
        }

        if (bspec.aspectNum > 0 && bspec.aspectDen > 0) {
            BVNodeId numNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(bspec.aspectNum)), 64);
            BVNodeId denNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(bspec.aspectDen)), 64);
            BVNodeId lhs = ctx.make_binary(BVOp::Mul, bn.w, denNode);
            BVNodeId rhs = ctx.make_binary(BVOp::Mul, bn.h, numNode);
            cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Eq, lhs, rhs));
        }

        boxMap[bspec.id] = bn;
        boxNodes.push_back(std::move(bn));
    }

    // Pairwise non-overlap disjunctions
    if (spec.nonOverlap && boxNodes.size() > 1) {
        BVNodeId gapNode = ctx.make_const(static_cast<uint64_t>(static_cast<int64_t>(spec.gap)), 64);
        for (size_t i = 0; i < boxNodes.size(); ++i) {
            for (size_t j = i + 1; j < boxNodes.size(); ++j) {
                const auto& a = boxNodes[i];
                const auto& b = boxNodes[j];
                BVNodeId aLeft = ctx.make_binary(
                    BVOp::Sle,
                    ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, a.x, a.w), gapNode),
                    b.x);
                BVNodeId bLeft = ctx.make_binary(
                    BVOp::Sle,
                    ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, b.x, b.w), gapNode),
                    a.x);
                BVNodeId aAbove = ctx.make_binary(
                    BVOp::Sle,
                    ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, a.y, a.h), gapNode),
                    b.y);
                BVNodeId bAbove = ctx.make_binary(
                    BVOp::Sle,
                    ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, b.y, b.h), gapNode),
                    a.y);

                BVNodeId disj = ctx.make_binary(
                    BVOp::Or,
                    ctx.make_binary(BVOp::Or, aLeft, bLeft),
                    ctx.make_binary(BVOp::Or, aAbove, bAbove));

                if (a.is3D && b.is3D) {
                    BVNodeId aFront = ctx.make_binary(
                        BVOp::Sle,
                        ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, a.z, a.d), gapNode),
                        b.z);
                    BVNodeId bFront = ctx.make_binary(
                        BVOp::Sle,
                        ctx.make_binary(BVOp::Add, ctx.make_binary(BVOp::Add, b.z, b.d), gapNode),
                        a.z);
                    disj = ctx.make_binary(BVOp::Or, disj, ctx.make_binary(BVOp::Or, aFront, bFront));
                }
                cond = ctx.make_binary(BVOp::And, cond, disj);
            }
        }
    }

    // Algebraic string constraints
    for (const auto& cstr : spec.constraints) {
        if (cstr.empty()) continue;
        ConstraintParser parser(cstr, ctx, boxMap, spec);
        BVNodeId cNode = 0;
        if (!parser.parse(cNode)) {
            sol.status = SolveStatus::Error;
            sol.error = "invalid layout constraint expression: '" + cstr + "'";
            return sol;
        }
        cond = ctx.make_binary(BVOp::And, cond, cNode);
    }

    // Optional JS predicate function over the box variables
    if (!spec.predicateFnSource.empty()) {
        std::string normFn = normalizeJsFunctionSource(spec.predicateFnSource, nullptr, "__layout_pred");
        il::Module predMod;
        std::string predErr;
        if (!lowerSingleSourceForEquiv(normFn, "layout_pred.js", predMod, &predErr)) {
            sol.status = SolveStatus::Error;
            sol.error = predErr;
            return sol;
        }
        const il::Function* predFn = nullptr;
        for (const auto& f : predMod.functions) {
            if (!f.isExternal && !f.isEntryPoint && f.name != "main") {
                predFn = &f;
                break;
            }
        }
        if (!predFn) {
            sol.status = SolveStatus::Error;
            sol.error = "predicate function not found";
            return sol;
        }
        BVNodeId predRet = 0;
        std::string reason;
        if (!evalIlFunctionSymbolic(ctx, predMod, *predFn, allVarNodes, true, &predRet, &reason)) {
            sol.status = SolveStatus::Error;
            sol.error = "predicate function not symbolically decidable: " + reason;
            return sol;
        }
        cond = ctx.make_binary(BVOp::And, cond, ctx.make_binary(BVOp::Ne, predRet, zero64));
    }

    SolveOutcome satRes = brass::verify::solve_nonzero(ctx, cond);
    auto t1 = std::chrono::steady_clock::now();
    sol.solveTimeMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    if (satRes.status == SolveOutcome::Unsat) {
        sol.status = SolveStatus::Unsat;
        return sol;
    }
    if (satRes.status != SolveOutcome::Sat) {
        sol.status = SolveStatus::Unknown;
        sol.error = "SMT solver returned UNKNOWN";
        return sol;
    }

    sol.status = SolveStatus::Sat;
    std::vector<uint64_t> model = satRes.model;
    if (model.size() < nextVarIdx) model.resize(nextVarIdx, 0);

    auto readS32 = [&](uint32_t idx) -> int64_t {
        return static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(model[idx])));
    };

    for (const auto& bn : boxNodes) {
        SolvedBox sb;
        sb.id = bn.id;
        sb.is3D = bn.is3D;
        sb.x = readS32(bn.idxX);
        sb.y = readS32(bn.idxY);
        sb.w = readS32(bn.idxW);
        sb.h = readS32(bn.idxH);
        if (bn.is3D) {
            sb.z = readS32(bn.idxZ);
            sb.d = readS32(bn.idxD);
        }
        sol.vars[bn.id + ".x"] = sb.x;
        sol.vars[bn.id + ".y"] = sb.y;
        sol.vars[bn.id + ".w"] = sb.w;
        sol.vars[bn.id + ".h"] = sb.h;
        sol.vars[bn.id + "_x"] = sb.x;
        sol.vars[bn.id + "_y"] = sb.y;
        sol.vars[bn.id + "_w"] = sb.w;
        sol.vars[bn.id + "_h"] = sb.h;
        if (bn.is3D) {
            sol.vars[bn.id + ".z"] = sb.z;
            sol.vars[bn.id + ".d"] = sb.d;
            sol.vars[bn.id + "_z"] = sb.z;
            sol.vars[bn.id + "_d"] = sb.d;
        }
        sol.boxes[sb.id] = sb;
        sol.boxList.push_back(std::move(sb));
    }

    return sol;
}

LayoutSolution solveLayoutPredicate(const std::vector<std::string>& varNames,
                                    std::string_view constraintFnSource,
                                    const SolveOptions& opts) {
    LayoutSolution sol;
    SolveResult sres = solveJsFunction(constraintFnSource, opts);
    sol.status = sres.status;
    sol.solveTimeMs = sres.solveTimeMs;
    sol.error = sres.error;
    if (!sres.sat()) return sol;

    std::vector<std::string> boxOrder;
    for (size_t i = 0; i < sres.args.size(); ++i) {
        std::string vname = (i < varNames.size() && !varNames[i].empty())
                                ? varNames[i]
                                : (i < sres.paramNames.size() ? sres.paramNames[i]
                                                              : ("arg_" + std::to_string(i)));
        int64_t val = sres.args[i];
        sol.vars[vname] = val;

        size_t sep = vname.find('.');
        if (sep == std::string::npos) sep = vname.rfind('_');
        if (sep != std::string::npos && sep > 0 && sep + 1 < vname.size()) {
            std::string boxId = vname.substr(0, sep);
            std::string prop = vname.substr(sep + 1);
            if (prop == "x" || prop == "y" || prop == "z" ||
                prop == "w" || prop == "h" || prop == "d") {
                if (sol.boxes.find(boxId) == sol.boxes.end()) {
                    SolvedBox sb;
                    sb.id = boxId;
                    sol.boxes[boxId] = sb;
                    boxOrder.push_back(boxId);
                }
                SolvedBox& sb = sol.boxes[boxId];
                if (prop == "x") sb.x = val;
                else if (prop == "y") sb.y = val;
                else if (prop == "z") { sb.z = val; sb.is3D = true; }
                else if (prop == "w") sb.w = val;
                else if (prop == "h") sb.h = val;
                else if (prop == "d") { sb.d = val; sb.is3D = true; }
            }
        }
    }

    for (const auto& id : boxOrder) {
        sol.boxList.push_back(sol.boxes[id]);
    }
    return sol;
}

}  // namespace bronze::check
