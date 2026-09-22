#pragma once

#include <brass/object/object_writer.hpp>
#include "il/il.h"
#include <string>
#include <vector>

namespace bronze::codegen {

void translateDebugLocations(
    brass::object::ObjectFile& obj,
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    const std::string& entrySymbol
);

void emitNativeDebugSections(
    brass::object::ObjectFile& obj,
    const il::Module& module,
    const std::vector<std::string>& uniqueNames,
    const std::string& entrySymbol,
    bool emitDebugInfo
);

}  // namespace bronze::codegen
