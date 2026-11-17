#pragma once

// `bronze build --size-report`: where an image's bytes go, printed to stderr
// once the object is complete (emitBronzeSections' last step).
//
// Per section; then the functions' code with the tables brass and bronze keep
// per function (stack map, pc table, inline frames), by copy kind (the
// generic function G, the speculated copy S `$spec`, the optimistic copy S+
// `$opt`, the resume twin `__twin`, the entry and its segments, and the thunks
// the backend adds) and by what the spent harvest said of the function's run
// (il.h Function::heat); then the module-wide tables by symbol; then the
// largest functions. Like --timings, a debugging report: on request only,
// compared by nothing.

#include <string>
#include <vector>

#include <brass/object/object_writer.hpp>

#include "il/il.h"

namespace bronze::codegen {

bool sizeReportEnabled();
void setSizeReportEnabled(bool on);

void printSizeReport(const brass::object::ObjectFile& obj, const il::Module& module,
                     const std::vector<std::string>& uniqueNames, const std::string& entrySymbol);

}  // namespace bronze::codegen
