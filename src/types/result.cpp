#include "types/result.h"

namespace bronze::types {
namespace {

// Every out-of-range or unproven query lands here rather than on a caller's
// error path: not knowing is the normal answer, and the uniform dynamic
// convention is what "not knowing" means at a call boundary.
const Signature& dynamicSignature() {
    static const Signature kDynamic = [] {
        Signature s;
        s.returnType = Type::dynamic();
        return s;
    }();
    return kDynamic;
}

}  // namespace

Type InferenceResult::typeAt(const ast::Expr* expr) const {
    const auto it = exprTypes.find(expr);
    return it == exprTypes.end() ? Type::dynamic() : it->second;
}

ShapeClassId InferenceResult::shapeClassAt(const ast::Expr* site) const {
    const auto it = siteShapes.find(site);
    return it == siteShapes.end() ? kNoShapeClass : it->second;
}

const Signature& InferenceResult::signatureOf(uint32_t functionIndex) const {
    if (functionIndex >= moduleSignatures.size()) return dynamicSignature();
    return moduleSignatures[functionIndex];
}

bool InferenceResult::isDirectCallable(uint32_t functionIndex) const {
    if (functionIndex >= moduleDirectCallable.size()) return false;
    return moduleDirectCallable[functionIndex];
}

bool InferenceResult::isDirectCallable(const std::string& name) const {
    const auto index = functionIndexOf(name);
    return index.has_value() && isDirectCallable(*index);
}

std::optional<uint32_t> InferenceResult::functionIndexOf(const std::string& name) const {
    const auto it = moduleFunctionIndex.find(name);
    if (it == moduleFunctionIndex.end()) return std::nullopt;
    return it->second;
}

}  // namespace bronze::types
