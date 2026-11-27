#pragma once

// Stable names for shapes, heap kinds, and compiled functions, used by
// inline-cache harvesting (`runtime/ic_harvest.h`) and shape cells
// (`runtime/shape_cells.h`).

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bronze {
class Shape;
}

namespace bronze::runtime {

// `<prototype>[#N]{k1,...}` (`protoNames` caches the prototype part by root shape).
std::string typeFeedbackShapeName(const Shape* shape, std::unordered_map<const Shape*, std::string>& protoNames);
const char* typeFeedbackKindWord(uint16_t flags);
// `~<source offset>@<hash of source text>`, or empty when no source was registered.
std::string typeFeedbackCodeName(const void* code);

}  // namespace bronze::runtime
