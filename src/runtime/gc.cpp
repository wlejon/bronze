#include "runtime/gc.h"

#include "abi/bronze_abi.h"
#include "runtime/fatal.h"

// Generated code's own frame chain lives in the per-thread bronze_tls_block
// (tls_block.cpp, `frame_top`): compiled code links and unlinks against the
// block its prologue fetched, and the collector walks the calling thread's
// chain (heap.cpp). What remains here is the C++ side's shadow stack.

namespace bronze {

thread_local ShadowStackFrame* ShadowStackFrame::top_frame_ = nullptr;

ShadowStackFrame::ShadowStackFrame() noexcept {
    prev_ = top_frame_;
    top_frame_ = this;
}

ShadowStackFrame::~ShadowStackFrame() noexcept {
    top_frame_ = prev_;
}

ShadowStackFrame* ShadowStackFrame::current() noexcept {
    return top_frame_;
}

ShadowStackFrame* requireFrameForRoot() {
    ShadowStackFrame* frame = ShadowStackFrame::current();
    if (!frame) {
        fatal("Rooted<> with no ShadowStackFrame open: every entry into bronze opens "
              "one (rt.cpp's main, embed::runMain, embed::runEntry, and each embed:: "
              "API function), and generated code is only reached through them — a host "
              "calling raw bronze_* helpers must open a bronze::ShadowStackFrame first");
    }
    return frame;
}

void ShadowStackFrame::push(Value* slot) {
    roots_.push_back(slot);
}

void ShadowStackFrame::pop(Value* slot) {
    if (!roots_.empty()) {
        if (roots_.back() == slot) {
            roots_.pop_back();
        } else {
            for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
                if (*it == slot) {
                    roots_.erase(std::next(it).base());
                    break;
                }
            }
        }
    }
}

}  // namespace bronze
