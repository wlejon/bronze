#pragma once

// THE TYPE-FEEDBACK INSTRUMENT, lowering half (`bronze build --feedback-out`,
// docs/type-feedback.md).
//
// A feedback build is an instrument: it carries one `feedback.record` per site
// il/feedback_site.h enumerates, and a representative run of it writes a
// profile of what those sites saw — value tags, receiver shapes, callees. A
// later build reads the profile back (`--feedback-in`) and asks about sites by
// the same (function key, ordinal) pairs, which is why the instrumentation is
// the LAST step of lowering: the sites must be the final IL's own, and no pass
// may see a record and change around it.

#include <string>

#include "il/il.h"

namespace bronze::spec {
class TypeProfile;
struct FeedbackPlan;
}

namespace bronze::lower {

// What `lowerModule` does about type feedback.
//
// `outPath` (`--feedback-out`) instruments the module so that a run writes its
// profile there (the runtime's BRONZE_FEEDBACK_OUT overrides it at run time).
// `profile` (`--feedback-in`) is a profile read back, which the final passes
// specialize on (lower_final_passes.cpp, opt/spec/feedback_plan.h).
struct FeedbackOptions {
    std::string outPath;
    const spec::TypeProfile* profile = nullptr;
};

struct FeedbackStats {
    uint32_t functions = 0;
    uint32_t sites = 0;
};

// Inserts the records and registers the output path through the module's
// census table (abi/bronze_feedback_site.h). False, with `error` set, when the
// module already carries a pin census: the two share one channel.
bool instrumentFeedback(il::Module& module, const std::string& outPath, std::string* error,
                        FeedbackStats* stats = nullptr);

}  // namespace bronze::lower
