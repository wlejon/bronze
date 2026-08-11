#include "types/shape_class.h"

#include <stdexcept>

namespace bronze::types {
namespace {

// A key that cannot collide across different (constructor, property list)
// pairs: property names are JS identifiers, so a byte no identifier can
// contain separates them.
std::string makeKey(const std::string& ctor, const std::vector<std::string>& props) {
    std::string key = ctor;
    for (const auto& p : props) {
        key += '\x1f';
        key += p;
    }
    return key;
}

}  // namespace

ShapeClassId ShapeClassTable::intern(std::string constructorName,
                                     std::vector<std::string> properties) {
    const std::string key = makeKey(constructorName, properties);
    const auto it = index_.find(key);
    if (it != index_.end()) return it->second;

    const auto id = static_cast<ShapeClassId>(classes_.size());
    classes_.push_back(ShapeClass{std::move(constructorName), std::move(properties)});
    index_.emplace(key, id);
    return id;
}

const ShapeClass& ShapeClassTable::at(ShapeClassId id) const {
    // Every id in circulation came out of intern(). Reaching here with
    // anything else is an internal impossibility, not a fallback case.
    if (id >= classes_.size()) {
        throw std::logic_error("bronze::types: shape class id out of range");
    }
    return classes_[id];
}

std::string ShapeClassTable::describe(ShapeClassId id) const {
    const ShapeClass& cls = at(id);
    std::string out = cls.constructorName;
    out += '{';
    for (size_t i = 0; i < cls.properties.size(); ++i) {
        if (i > 0) out += ", ";
        out += cls.properties[i];
    }
    out += '}';
    return out;
}

}  // namespace bronze::types
