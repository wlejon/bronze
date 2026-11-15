#include "opt/repr/encoding.h"

#include <cmath>

namespace bronze::opt::repr {

std::optional<BoxedForm> boxedForm(il::Type boxType) {
    BoxedForm form;
    // No box of a typed value is an object, nullish or the hole: those are
    // tags only a Dynamic value made elsewhere can carry.
    form.isObject = false;
    form.isNullish = false;
    form.isHole = false;
    switch (boxType) {
        case il::Type::I32:
            // The Int32 tag.
            form.isI32 = true;
            form.isNumber = false;
            form.unboxF64ReadsValue = false;
            return form;
        case il::Type::F64:
            // The double box: always `is.number`; `is.i32` only for an
            // integral value, which the caller decides from the operand.
            form.isI32 = std::nullopt;
            form.isNumber = true;
            form.unboxF64ReadsValue = true;
            return form;
        case il::Type::Bool:
        case il::Type::Str:
            form.isI32 = false;
            form.isNumber = false;
            form.unboxF64ReadsValue = false;
            return form;
        default:
            return std::nullopt;
    }
}

bool isInt32Double(double d) {
    if (!std::isfinite(d) || d != std::trunc(d)) return false;
    if (d < -2147483648.0 || d > 2147483647.0) return false;
    return !(d == 0.0 && std::signbit(d));
}

}  // namespace bronze::opt::repr
