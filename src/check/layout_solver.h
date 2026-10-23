#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "check/solve_fn.h"

namespace bronze::check {

struct LayoutBoxSpec {
    std::string id;
    bool is3D = false;
    int32_t minX = 0;
    int32_t maxX = 4096;
    int32_t minY = 0;
    int32_t maxY = 4096;
    int32_t minZ = 0;
    int32_t maxZ = 4096;
    int32_t minW = 1;
    int32_t maxW = 4096;
    int32_t minH = 1;
    int32_t maxH = 4096;
    int32_t minD = 1;
    int32_t maxD = 4096;
    int32_t snapGrid = 0;
    int32_t aspectNum = 0;
    int32_t aspectDen = 0;
};

struct LayoutSpec {
    bool hasContainer = true;
    int32_t containerW = 1920;
    int32_t containerH = 1080;
    int32_t containerD = 1000;
    bool nonOverlap = false;
    int32_t gap = 0;
    int32_t globalSnapGrid = 0;
    std::vector<LayoutBoxSpec> boxes;
    std::vector<std::string> constraints;
    std::string predicateFnSource;
};

struct SolvedBox {
    std::string id;
    int64_t x = 0;
    int64_t y = 0;
    int64_t z = 0;
    int64_t w = 0;
    int64_t h = 0;
    int64_t d = 0;
    bool is3D = false;
};

struct LayoutSolution {
    SolveStatus status = SolveStatus::Unknown;
    std::vector<SolvedBox> boxList;
    std::unordered_map<std::string, SolvedBox> boxes;
    std::unordered_map<std::string, int64_t> vars;
    double solveTimeMs = 0.0;
    std::string error;

    bool sat() const { return status == SolveStatus::Sat; }
    bool unsat() const { return status == SolveStatus::Unsat; }
};

// Solves a declarative non-convex 2D/3D layout specification (`boxes`,
// `container`, `nonOverlap` disjunctions, modulo `snapGrid`, `aspectRatio`,
// algebraic string `constraints`, and optional JS `predicateFnSource`) using
// Brass's bitvector CDCL SMT solver.
LayoutSolution solveLayoutSpec(const LayoutSpec& spec);

// Solves a layout specified as a list of variable names + a JS constraint
// predicate function `(v0, v1, ...) => ...`. Automatically populates both
// `solution.vars` and `solution.boxes` (for variables named `<id>_<x|y|z|w|h|d>`).
LayoutSolution solveLayoutPredicate(const std::vector<std::string>& varNames,
                                    std::string_view constraintFnSource,
                                    const SolveOptions& opts = {});

}  // namespace bronze::check
