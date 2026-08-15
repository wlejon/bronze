#include "runtime/gc.h"

#include "abi/bronze_abi.h"

extern "C" {
bronze_gc_frame* bronze_gc_frame_top = nullptr;
}

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
