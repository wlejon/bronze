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

uint32_t InferenceResult::staticSlotAt(const ast::Expr* receiver,
                                      const std::string& field) const {
    const Type t = typeAt(receiver);
    if (!t.is(TypeKind::Object)) return ClassLayoutTable::kNoSlot;
    return classLayouts.slotOf(t.shapeClass(), field);
}

ShapeClassId InferenceResult::guessedParamShapeAt(const ast::Expr* receiver) const {
    const auto it = guessedParamShapes.find(receiver);
    return it == guessedParamShapes.end() ? kNoShapeClass : it->second;
}

Type InferenceResult::typeOfBindingAt(const ast::Stmt* mergePoint,
                                      const std::string& name) const {
    const auto point = mergeBindings.find(mergePoint);
    if (point == mergeBindings.end()) return Type::dynamic();
    const auto binding = point->second.find(name);
    return binding == point->second.end() ? Type::dynamic() : binding->second;
}

Type InferenceResult::closureReturnAt(const ast::Node* site) const {
    const auto it = closureReturns.find(site);
    return it == closureReturns.end() ? Type::dynamic() : it->second;
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
