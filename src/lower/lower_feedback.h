#pragma once

// THE TYPE-FEEDBACK INSTRUMENT, lowering half (`bronze build --feedback-out`,
// docs/type-feedback.md).
//
// A feedback build is an instrument: it carries one `feedback.record` per site
// il/feedback_site.h enumerates, and a representative run of it writes a
// profile of what those sites saw — value tags, receiver shapes, callees. The
// sites are named by (function key, ordinal) pairs, which is why the
// instrumentation is the LAST step of lowering: the sites must be the final
// IL's own, and no pass may see a record and change around it.

#include <string>

#include "il/il.h"

namespace bronze::lower {

// What `lowerModule` does about type feedback.
//
// `outPath` (`--feedback-out`) instruments the module so that a run writes its
// profile there (the runtime's BRONZE_FEEDBACK_OUT overrides it at run time).
struct FeedbackOptions {
    std::string outPath;
};

struct FeedbackStats {
    uint32_t functions = 0;
    uint32_t sites = 0;
};

// Inserts the records and registers the output path through the module's
// census table (abi/bronze_feedback_site.h).
void instrumentFeedback(il::Module& module, const std::string& outPath, FeedbackStats* stats = nullptr);

}  // namespace bronze::lower
