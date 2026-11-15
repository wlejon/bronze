// The shape test every own-slot rewrite begins with:
//
//     t = is.object %obj          ; guard: an object, so its shape word exists
//     s = load.shape %obj
//     c = shape.cell <n>          ; the module's cell for the profile's name
//     e = ptr.eq s, c             ; guard: exactly that shape
//
// `is.object` is total and pure and `load.shape` reads the header word every
// object kind has at BRONZE_ABI_OBJ_SHAPE_OFFSET, so nothing here is
// observable: a failure at either guard leaves before any effect. The compare
// stays in the block of its branch with the load, which is where edge_facts
// recognises it as a shape fact.

#include "opt/speculate/sites.h"

namespace bronze::opt::speculate {

ValueId emitShapeOf(GuardBuilder& b, ValueId object) {
    assert(b.typeOf(object) == il::Type::Dynamic);
    const ValueId isObject = b.emit(il::Op::IsObject, il::Type::Bool, {object});
    b.guard(isObject);
    return b.emit(il::Op::LoadShape, il::Type::Ptr, {object});
}

ValueId emitShapeIs(GuardBuilder& b, ValueId shape, const std::string& key) {
    const uint32_t cell = b.context().cells.cellFor(key);
    const ValueId expected = b.emit(il::Op::ShapeCell, il::Type::Ptr, {}, static_cast<int32_t>(cell));
    return b.emit(il::Op::PtrEq, il::Type::Bool, {shape, expected});
}

void guardShape(GuardBuilder& b, ValueId object, const std::string& key) {
    const ValueId shape = emitShapeOf(b, object);
    b.guard(emitShapeIs(b, shape, key));
}

}  // namespace bronze::opt::speculate
