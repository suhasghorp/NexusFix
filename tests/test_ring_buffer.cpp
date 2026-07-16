#include <catch2/catch_test_macros.hpp>
#include <array>
#include <cstring>
#include <string_view>

#include "nexusfix/transport/socket.hpp"

using namespace nfx;

// ============================================================================
// RingBuffer Unit Tests
// ============================================================================

TEST_CASE("RingBuffer: initial state", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    CHECK(buf.empty());
    CHECK_FALSE(buf.full());
    CHECK(buf.size() == 0);
    CHECK(buf.available() == 64);
}

TEST_CASE("RingBuffer: write then read preserves data", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    const char data[] = "Hello, RingBuffer!";
    size_t written = buf.write({data, sizeof(data) - 1});
    REQUIRE(written == sizeof(data) - 1);
    CHECK(buf.size() == written);
    CHECK_FALSE(buf.empty());

    std::array<char, 64> out{};
    size_t read = buf.read({out.data(), out.size()});
    REQUIRE(read == written);
    CHECK(std::string_view(out.data(), read) == "Hello, RingBuffer!");
    CHECK(buf.empty());
}

TEST_CASE("RingBuffer: write to full buffer returns 0 for overflow", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    // Fill completely
    std::array<char, 64> fill{};
    std::memset(fill.data(), 'A', fill.size());
    size_t written = buf.write({fill.data(), fill.size()});
    REQUIRE(written == 64);
    CHECK(buf.full());
    CHECK(buf.available() == 0);

    // Additional write returns 0
    const char extra[] = "more";
    size_t overflow = buf.write({extra, 4});
    CHECK(overflow == 0);
    CHECK(buf.size() == 64);
}

TEST_CASE("RingBuffer: read from empty buffer returns 0", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    std::array<char, 16> out{};
    size_t read = buf.read({out.data(), out.size()});
    CHECK(read == 0);
}

TEST_CASE("RingBuffer: wraparound correctness", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    // Fill buffer
    std::array<char, 48> first{};
    std::memset(first.data(), 'A', first.size());
    REQUIRE(buf.write({first.data(), first.size()}) == 48);

    // Drain half (24 bytes) - head advances
    std::array<char, 24> drain{};
    REQUIRE(buf.read({drain.data(), drain.size()}) == 24);
    CHECK(buf.size() == 24);

    // Write 40 more bytes - wraps around
    std::array<char, 40> second{};
    std::memset(second.data(), 'B', second.size());
    REQUIRE(buf.write({second.data(), second.size()}) == 40);
    CHECK(buf.size() == 64);

    // Drain all and verify order: 24 bytes of 'A' then 40 bytes of 'B'
    std::array<char, 64> out{};
    size_t total = buf.read({out.data(), out.size()});
    REQUIRE(total == 64);
    for (size_t i = 0; i < 24; ++i) {
        CHECK(out[i] == 'A');
    }
    for (size_t i = 24; i < 64; ++i) {
        CHECK(out[i] == 'B');
    }
    CHECK(buf.empty());
}

TEST_CASE("RingBuffer: read_span returns contiguous view", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    const char data[] = "contiguous";
    (void)buf.write({data, 10});

    auto span = buf.read_span();
    REQUIRE(span.size() == 10);
    CHECK(std::string_view(span.data(), span.size()) == "contiguous");

    // read_span does not consume
    CHECK(buf.size() == 10);
}

TEST_CASE("RingBuffer: write_span and commit_write", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    auto ws = buf.write_span();
    REQUIRE(ws.size() > 0);

    // Write directly into the span
    const char data[] = "direct";
    std::memcpy(ws.data(), data, 6);
    buf.commit_write(6);

    CHECK(buf.size() == 6);

    std::array<char, 16> out{};
    size_t read = buf.read({out.data(), out.size()});
    REQUIRE(read == 6);
    CHECK(std::string_view(out.data(), read) == "direct");
}

TEST_CASE("RingBuffer: peek reads without consuming", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    const char data[] = "peekable";
    (void)buf.write({data, 8});

    std::array<char, 16> out{};
    size_t peeked = buf.peek({out.data(), out.size()});
    REQUIRE(peeked == 8);
    CHECK(std::string_view(out.data(), peeked) == "peekable");

    // Size unchanged after peek
    CHECK(buf.size() == 8);

    // Can still read the same data
    std::array<char, 16> out2{};
    size_t read = buf.read({out2.data(), out2.size()});
    REQUIRE(read == 8);
    CHECK(std::string_view(out2.data(), read) == "peekable");
    CHECK(buf.empty());
}

TEST_CASE("RingBuffer: skip advances head without copying", "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    const char data[] = "skip_me_keep_this";
    (void)buf.write({data, 17});
    CHECK(buf.size() == 17);

    // Skip first 8 bytes ("skip_me_")
    buf.skip(8);
    CHECK(buf.size() == 9);

    std::array<char, 16> out{};
    size_t read = buf.read({out.data(), out.size()});
    REQUIRE(read == 9);
    CHECK(std::string_view(out.data(), read) == "keep_this");
}

// ============================================================================
// TICKET_497 Phase 1: wraparound / boundary branches of read_span, write_span,
// skip and commit_write that the happy-path tests above do not reach.
// ============================================================================

TEST_CASE("RingBuffer: read_span wrap branch returns only first segment",
          "[transport][ring_buffer][regression]") {
    RingBuffer<8> buf;

    // Advance head past 0 so a later write wraps the tail behind it.
    std::array<char, 6> pre{};
    std::memset(pre.data(), 'X', pre.size());
    REQUIRE(buf.write({pre.data(), pre.size()}) == 6);
    std::array<char, 6> drop{};
    REQUIRE(buf.read({drop.data(), drop.size()}) == 6);  // head = 6

    // Write 4 bytes: occupies indices 6,7 then wraps to 0,1 (start=6 > end=2).
    const char data[] = "WXYZ";
    REQUIRE(buf.write({data, 4}) == 4);
    CHECK(buf.size() == 4);

    // read_span hits the `else if (size() > 0)` wrap branch: first segment only,
    // from start (6) to end of buffer (Size 8) = 2 bytes.
    auto span = buf.read_span();
    REQUIRE(span.size() == 2);
    CHECK(span[0] == 'W');
    CHECK(span[1] == 'X');
}

TEST_CASE("RingBuffer: read_span on empty buffer returns empty span",
          "[transport][ring_buffer][regression]") {
    RingBuffer<8> buf;
    // start == end and size() == 0: falls through both branches to `return {}`.
    auto span = buf.read_span();
    CHECK(span.size() == 0);
}

TEST_CASE("RingBuffer: write_span wrap branch caps at buffer end",
          "[transport][ring_buffer][regression]") {
    RingBuffer<8> buf;

    // Move tail to index 6, then free space at the front so avail wraps.
    std::array<char, 6> pre{};
    std::memset(pre.data(), 'A', pre.size());
    REQUIRE(buf.write({pre.data(), pre.size()}) == 6);  // tail = 6
    std::array<char, 4> drop{};
    REQUIRE(buf.read({drop.data(), drop.size()}) == 4);  // head = 4, size = 2

    // available() == 6, but tail index is 6, so end(6)+avail(6) > Size(8):
    // write_span returns only the tail-to-end segment (Size - end = 2 bytes).
    auto ws = buf.write_span();
    REQUIRE(ws.size() == 2);
}

TEST_CASE("RingBuffer: commit_write caps at available space",
          "[transport][ring_buffer][regression]") {
    RingBuffer<8> buf;

    // Request to commit more than available: commit_write clamps via std::min.
    (void)buf.write_span();
    buf.commit_write(100);
    CHECK(buf.size() == 8);   // clamped to capacity, never overflows
    CHECK(buf.full());
}

TEST_CASE("RingBuffer: skip past available clamps to size",
          "[transport][ring_buffer][regression]") {
    RingBuffer<64> buf;

    const char data[] = "short";
    REQUIRE(buf.write({data, 5}) == 5);

    // Skip more than present: head advances by std::min(count, available).
    buf.skip(100);
    CHECK(buf.size() == 0);
    CHECK(buf.empty());
}

// ============================================================================
// connection_state_name runtime lookup (socket.hpp) including out-of-range.
// ============================================================================

TEST_CASE("connection_state_name runtime lookup", "[transport][socket][regression]") {
    CHECK(connection_state_name(ConnectionState::Disconnected) == "Disconnected");
    CHECK(connection_state_name(ConnectionState::Connecting) == "Connecting");
    CHECK(connection_state_name(ConnectionState::Connected) == "Connected");
    CHECK(connection_state_name(ConnectionState::Disconnecting) == "Disconnecting");
    CHECK(connection_state_name(ConnectionState::Error) == "Error");

    // Out-of-range enum value takes the `return "Unknown"` fallthrough branch.
    CHECK(connection_state_name(static_cast<ConnectionState>(200)) == "Unknown");
}
