#pragma once

// The front end of `bronze auto-kernel` / `bro.autoKernel` (#161): it parses the
// function, finds its canonical `for` loop in the AST, and walks EVERY statement
// and expression of the loop body (nested blocks included), collecting the
// element reads and writes with their indices as affine forms. It decides
// nothing about races; auto_kernel_rewriter.cpp asks the solver about the
// accesses this collects, and prints the kernel from `bodyText`.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace bronze::check::autokernel {

// The atom naming the loop's induction variable in an Affine. Every other atom
// is a parameter name, or `<param>.length` (a dot cannot occur in a name).
inline constexpr const char* kIvAtom = "$iv";

// constant + sum(coefficient * atom), exact over the integers.
struct Affine {
    int64_t constant = 0;
    std::map<std::string, int64_t> terms;  // no zero coefficients

    int64_t coeff(const std::string& atom) const {
        auto it = terms.find(atom);
        return it == terms.end() ? 0 : it->second;
    }
    bool isConstant() const { return terms.empty(); }
};

// The form printed back as JavaScript, with the induction variable spelled `iv`.
std::string printAffine(const Affine& a, const std::string& iv);

struct ElementAccess {
    std::string array;      // the parameter indexed
    std::string indexText;  // the index as written
    bool isWrite = false;
    bool compound = false;  // `+=`, `++`, ...: this write also reads the element
    bool affine = false;
    Affine index;
    std::string whyNotAffine;
};

struct LoopAnalysis {
    // The source did not parse, or held no function.
    std::string error;
    std::string functionName;
    std::vector<std::string> params;

    bool foundLoop = false;
    std::string inductionVar;
    std::string startText;
    std::string boundText;
    Affine start;
    Affine bound;
    bool inclusive = false;  // `<=` rather than `<`

    // Every element access in the body, in source order. A compound write is
    // ONE entry with `compound` set (it is both a read and a write).
    std::vector<ElementAccess> accesses;

    // The first reason the body cannot become a kernel, from the AST: empty
    // when there is none. `rejectKind` is "SIDE_EFFECT", "NON_AFFINE" or
    // "UNSUPPORTED".
    std::string rejectKind;
    std::string rejectReason;
    std::string rejectArray;
    std::string rejectIndex;

    // The loop body exactly as written (between its braces, or the single
    // statement), checked to parse back to the same tree as the analysed body.
    // Empty when that check failed.
    std::string bodyText;
};

// `source` is a module holding the function (normalizeJsFunctionSource's output).
LoopAnalysis analyzeLoopSource(std::string_view source);

}  // namespace bronze::check::autokernel
