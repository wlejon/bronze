#pragma once
#include <cstdint>
#include <string>
#include <string_view>

namespace bronze {

// An immutable, named source buffer. Offsets are byte offsets; line/column
// queries are computed on demand (1-based, columns in bytes).
class SourceBuffer {
public:
    SourceBuffer(std::string name, std::string text)
        : name_(std::move(name)), text_(std::move(text)) {}

    std::string_view name() const { return name_; }
    std::string_view text() const { return text_; }

    struct LineCol {
        uint32_t line;
        uint32_t column;
    };
    LineCol lineCol(uint32_t offset) const;

private:
    std::string name_;
    std::string text_;
};

// Half-open byte range [begin, end) into one SourceBuffer.
struct Span {
    uint32_t begin = 0;
    uint32_t end = 0;
};

}  // namespace bronze
