// The constants below the thread's block (abi/bronze_abi_tls.h,
// BRONZE_TLS_CONSTANTS): compiled code reads constant i at
// [pinned register + BRONZE_TLS_CONSTANT_OFF(i)], and the pinned register is
// what bronze_tls_block_addr returns, so the word there must be the list's
// value, on every thread.

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "abi/bronze_abi.h"
#include "runtime/tls_block.h"

namespace {

std::vector<uint64_t> expectedConstants() {
    std::vector<uint64_t> v;
#define BRONZE_TEST_CONSTANT(x) v.push_back(static_cast<uint64_t>(x));
    BRONZE_TLS_CONSTANTS(BRONZE_TEST_CONSTANT)
#undef BRONZE_TEST_CONSTANT
    return v;
}

void checkBelowBlock() {
    const std::vector<uint64_t> want = expectedConstants();
    REQUIRE_EQ(want.size(), size_t(BRONZE_TLS_CONSTANT_COUNT));
    const char* base = reinterpret_cast<const char*>(bronze_tls_block_addr());
    CHECK_EQ(base, reinterpret_cast<const char*>(bronze::runtime::rtTls()));
    for (size_t i = 0; i < want.size(); ++i) {
        const int32_t off = BRONZE_TLS_CONSTANT_OFF(static_cast<int32_t>(i));
        CHECK(off >= -128);
        CHECK(off <= -8);
        uint64_t got = 0;
        std::memcpy(&got, base + off, sizeof got);
        CHECK_EQ(got, want[i]);
    }
}

}  // namespace

TEST_CASE("tls constants: each sits below the block at the offset compiled code reads") {
    checkBelowBlock();
}

TEST_CASE("tls constants: a new thread's block has them too") {
    std::thread t(checkBelowBlock);
    t.join();
}

TEST_CASE("tls constants: no value is listed twice") {
    const std::vector<uint64_t> want = expectedConstants();
    for (size_t i = 0; i < want.size(); ++i) {
        for (size_t j = i + 1; j < want.size(); ++j) CHECK_NE(want[i], want[j]);
    }
}
