#pragma once

#include <map>
#include <string>
#include <vector>

#include "types/type.h"

namespace bronze::types {

// One compile-time object identity: the ordered property names a site installs,
// plus the prototype source. Two sites agreeing on both share a class,
// mirroring the runtime's transition tree — the same reasoning that gave every
// `{}` literal one root shape, so a literal written twice is one class, not
// two.
struct ShapeClass {
    // Empty for an object literal (prototype `undefined`, one root shape); the
    // constructor's name for `new F()`, since the prototype is part of shape
    // identity.
    std::string constructorName;
    std::vector<std::string> properties;  // insertion order, deduplicated
};

class ShapeClassTable {
public:
    // Interns and returns the class id. Ids are handed out in first-intern
    // order, and the analysis walks the AST in source order, so they are
    // stable across runs.
    ShapeClassId intern(std::string constructorName, std::vector<std::string> properties);

    const ShapeClass& at(ShapeClassId id) const;
    const std::vector<ShapeClass>& all() const { return classes_; }
    size_t size() const { return classes_.size(); }

    // "{x, y}" or "Point{x, y}" — the dump's rendering of a class.
    std::string describe(ShapeClassId id) const;

private:
    std::vector<ShapeClass> classes_;
    // Ordered on purpose: nothing iterates it today, and nothing ever gets
    // the chance to leak an unordered iteration into an output path.
    std::map<std::string, ShapeClassId> index_;
};

}  // namespace bronze::types
