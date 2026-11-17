#pragma once

// A code range's pc table (bronze_abi.h bronze_code_range::pc_table): its
// source positions encoded, each relative to the one before it, which a
// stack trace decodes from the start up to the pc it asks about.
//
// One entry: an unsigned LEB128 HEAD, `(pc_offset - previous pc_offset) << 2`
// with bit 0 set when the file changes and bit 1 when the inline frame does;
// the line minus the previous line, zigzag LEB128; the column, LEB128; then
// the new file and the new inline frame, LEB128, as the head flags them. The
// first entry's previous is all zeros. Entries ascend by pc_offset (the
// writer sorts them). About four bytes an entry, where the table of
// bronze_pc_entry structs it replaced took twenty.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "abi/bronze_abi.h"

namespace bronze::pctable {

inline void putVar(std::vector<uint8_t>& out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<uint8_t>(v | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<uint8_t>(v));
}

// `entries` in ascending pc_offset order (sorted here, stably, if not).
inline std::vector<uint8_t> encode(std::vector<bronze_pc_entry> entries) {
    std::stable_sort(entries.begin(), entries.end(),
                     [](const bronze_pc_entry& a, const bronze_pc_entry& b) { return a.pc_offset < b.pc_offset; });
    std::vector<uint8_t> out;
    out.reserve(entries.size() * 4);
    bronze_pc_entry prev{};
    for (const bronze_pc_entry& e : entries) {
        const bool file = e.file != prev.file;
        const bool frame = e.inline_frame != prev.inline_frame;
        putVar(out, (static_cast<uint64_t>(e.pc_offset - prev.pc_offset) << 2) | (file ? 1u : 0u) | (frame ? 2u : 0u));
        const int64_t dl = static_cast<int64_t>(e.line) - static_cast<int64_t>(prev.line);
        putVar(out, (static_cast<uint64_t>(dl) << 1) ^ static_cast<uint64_t>(dl >> 63));
        putVar(out, e.col);
        if (file) putVar(out, e.file);
        if (frame) putVar(out, e.inline_frame);
        prev = e;
    }
    return out;
}

// Reads the entries of a table of `count` back in order.
class Reader {
public:
    Reader(const uint8_t* table, uint32_t count) : p_(table), left_(table ? count : 0) {}

    // The next entry into `e` (which holds the previous one, as `next` left
    // it); false at the end.
    bool next(bronze_pc_entry& e) {
        if (left_ == 0) return false;
        --left_;
        const uint64_t head = var();
        e.pc_offset += static_cast<uint32_t>(head >> 2);
        const uint64_t zl = var();
        e.line = static_cast<uint32_t>(static_cast<int64_t>(e.line) + (static_cast<int64_t>(zl >> 1) ^ -static_cast<int64_t>(zl & 1)));
        e.col = static_cast<uint32_t>(var());
        if (head & 1) e.file = static_cast<uint32_t>(var());
        if (head & 2) e.inline_frame = static_cast<uint32_t>(var());
        return true;
    }

private:
    uint64_t var() {
        uint64_t v = 0;
        for (int shift = 0;; shift += 7) {
            const uint8_t b = *p_++;
            v |= static_cast<uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0 || shift >= 63) return v;
        }
    }

    const uint8_t* p_;
    uint32_t left_;
};

// The entry in force at `pcOffset`: the last whose pc_offset is at most it.
// False when the first entry is past it (or there is none).
inline bool lookup(const uint8_t* table, uint32_t count, uint32_t pcOffset, bronze_pc_entry& out) {
    Reader r(table, count);
    bronze_pc_entry e{};
    bool found = false;
    while (r.next(e)) {
        if (e.pc_offset > pcOffset) break;
        out = e;
        found = true;
    }
    return found;
}

}  // namespace bronze::pctable
