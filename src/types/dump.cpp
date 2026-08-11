#include "types/dump.h"

#include <algorithm>

namespace bronze::types {
namespace {

// Function-body statements sit at depth 0 and are indented two spaces; each
// nesting level adds two more.
std::string indent(uint32_t depth) { return std::string(2 + depth * 2, ' '); }

void appendSignature(std::string& out, const FunctionFacts& fn) {
    out += "func " + fn.name + "(";
    const size_t count = std::min(fn.paramNames.size(), fn.signature.params.size());
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) out += ", ";
        out += fn.paramNames[i] + ": " + fn.signature.params[i].str();
    }
    out += ") -> " + fn.signature.returnType.str();
    if (fn.directCallable) out += " direct-callable";
    out += '\n';
}

// The env-backed bindings, ahead of the statements: they are one fact per
// function, so there is no statement to attach them to.
void appendCells(std::string& out, const FunctionFacts& fn) {
    if (fn.cells.empty()) return;
    out += indent(0) + "cells";
    for (size_t i = 0; i < fn.cells.size(); ++i) {
        out += i == 0 ? "  " : ", ";
        out += fn.cells[i].name + ": " + fn.cells[i].type.str();
    }
    out += '\n';
}

void appendStatement(std::string& out, const StatementFacts& s) {
    out += indent(s.depth);
    if (!s.isMarker) out += '#' + std::to_string(s.index) + ' ';
    out += s.label;
    for (size_t i = 0; i < s.changes.size(); ++i) {
        out += i == 0 ? "  " : ", ";
        out += s.changes[i].name + ": " + s.changes[i].type.str();
    }
    out += '\n';
}

}  // namespace

std::string dump(const InferenceResult& result) {
    std::string out = "module " + result.moduleName + '\n';
    for (const auto& fn : result.functions) {
        out += '\n';
        appendSignature(out, fn);
        appendCells(out, fn);
        for (const auto& s : fn.statements) appendStatement(out, s);
    }
    if (result.shapes.size() > 0) {
        out += "\nshapes\n";
        for (ShapeClassId id = 0; id < result.shapes.size(); ++id) {
            out += "  #" + std::to_string(id) + ' ' + result.shapes.describe(id) + '\n';
        }
    }
    return out;
}

}  // namespace bronze::types
