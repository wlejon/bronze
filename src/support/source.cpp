#include "support/source.h"

namespace bronze {

SourceBuffer::LineCol SourceBuffer::lineCol(uint32_t offset) const {
    LineCol lc{1, 1};
    const uint32_t limit = offset < text_.size() ? offset : static_cast<uint32_t>(text_.size());
    for (uint32_t i = 0; i < limit; ++i) {
        if (text_[i] == '\n') {
            ++lc.line;
            lc.column = 1;
        } else {
            ++lc.column;
        }
    }
    return lc;
}

}  // namespace bronze
