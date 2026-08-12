#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bronze {

// An immutable, named source buffer. Offsets are byte offsets; line/column
// queries are computed on demand (1-based, columns in bytes).
class SourceBuffer {
public:
    SourceBuffer(std::string name, std::string text, uint16_t fileId = 0)
        : name_(std::move(name)), text_(std::move(text)), fileId_(fileId) {}

    std::string_view name() const { return name_; }
    std::string_view text() const { return text_; }
    // Which file of the program this is. The lexer stamps it onto every token
    // span so that a diagnostic about a byte offset knows which text to count
    // lines in.
    uint16_t fileId() const { return fileId_; }

    struct LineCol {
        uint32_t line;
        uint32_t column;
    };
    LineCol lineCol(uint32_t offset) const;

private:
    std::string name_;
    std::string text_;
    uint16_t fileId_ = 0;
};

// Half-open byte range [begin, end) into one SourceBuffer of the program.
struct Span {
    uint32_t begin = 0;
    uint32_t end = 0;
    // Which buffer the offsets are into. Defaulted, and the entry file is
    // always 0, so every span written before bronze had a module graph — and
    // every `Span{}` a later stage builds for a diagnostic it has no source
    // location for — still names the file a single-file build compiles.
    uint16_t file = 0;
};

// Every buffer one compilation reads. A build used to be one file and a
// diagnostic rendered against that one buffer; with a module graph a span's
// offsets mean nothing without knowing which text they index, and rendering
// them against the wrong one produces a real-looking line and column
// pointing at unrelated code.
//
// Buffers are held by pointer so that adding one never invalidates a
// reference the parser or the lexer is still holding.
class SourceSet {
public:
    const SourceBuffer& add(std::string name, std::string text) {
        buffers_.push_back(std::make_unique<SourceBuffer>(
            std::move(name), std::move(text), static_cast<uint16_t>(buffers_.size())));
        return *buffers_.back();
    }

    // The buffer a span names, or the entry buffer when the id is out of
    // range — a diagnostic must still render, and file 0 is the file the
    // user named on the command line.
    const SourceBuffer& at(uint16_t id) const {
        return id < buffers_.size() ? *buffers_[id] : *buffers_.front();
    }

    bool empty() const { return buffers_.empty(); }
    size_t size() const { return buffers_.size(); }

private:
    std::vector<std::unique_ptr<SourceBuffer>> buffers_;
};

}  // namespace bronze
