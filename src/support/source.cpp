#include "support/source.h"

#include <algorithm>

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

LineTable::LineTable(std::string_view text) : size_(static_cast<uint32_t>(text.size())) {
    lineStarts_.push_back(0);
    for (uint32_t i = 0; i < size_; ++i) {
        if (text[i] == '\n') lineStarts_.push_back(i + 1);
    }
}

SourceBuffer::LineCol LineTable::lineCol(uint32_t offset) const {
    if (lineStarts_.empty()) return {1, 1};
    const uint32_t clamped = std::min(offset, size_);
    // The last line start at or before `offset`: the same answer the scan
    // gives, including for an offset sitting on the '\n' itself (column past
    // the line's last character, line unchanged).
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), clamped);
    --it;
    return {static_cast<uint32_t>(it - lineStarts_.begin()) + 1, clamped - *it + 1};
}

}  // namespace bronze
