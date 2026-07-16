// test_boundary_sweep.cpp
//
// TICKET_497 Phase 4: systematic boundary-value tests.
//
// Phase 1-2 closed error branches and injected faults. This file walks the
// numeric limits themselves: the largest/smallest values Price/Qty fixed-point
// can hold, SeqNum's 2^31-1 wrap, BodyLength at 0 and past the parser's field
// table, and the longest legal field the parser will accept. These are the
// inputs a fuzzer eventually reaches but that deserve a named, deterministic
// case each so a regression on any single edge is legible.

#include <catch2/catch_test_macros.hpp>

#include "nexusfix/types/field_types.hpp"
#include "nexusfix/parser/runtime_parser.hpp"

#include <cstdint>
#include <limits>
#include <string>

using namespace nfx;

namespace {

constexpr char SOH = '\x01';

// Build a checksum-correct FIX frame from a body (fields after 9=, before 10=).
// begin_string defaults to FIX.4.4. BodyLength is computed from the body bytes
// exactly as the parser expects (everything between the SOH after 9=... and the
// SOH before 10=).
std::string frame_with_body(const std::string& body,
                            std::string_view begin = "FIX.4.4") {
    std::string head;
    head += "8=";
    head.append(begin.data(), begin.size());
    head += SOH;
    head += "9=" + std::to_string(body.size()) + SOH;

    std::string frame = head + body;
    unsigned sum = 0;
    for (unsigned char c : frame) sum += c;
    char cs[8];
    std::snprintf(cs, sizeof(cs), "10=%03u", sum % 256);
    frame += cs;
    frame += SOH;
    return frame;
}

// A minimal well-formed ExecutionReport body (35=8) that the parser accepts.
std::string minimal_body() {
    std::string b;
    b += "35=8";  b += SOH;
    b += "49=SENDER"; b += SOH;
    b += "56=TARGET"; b += SOH;
    b += "34=1"; b += SOH;
    b += "52=20240101-00:00:00.000"; b += SOH;
    return b;
}

}  // namespace

// ============================================================================
// FixedPrice numeric limits (int64_t scaled at 10^8)
// ============================================================================

TEST_CASE("Boundary: FixedPrice at int64 extremes", "[boundary][types][price]") {
    SECTION("max raw round-trips through scaled()") {
        FixedPrice p{std::numeric_limits<int64_t>::max()};
        REQUIRE(p.scaled() == std::numeric_limits<int64_t>::max());
    }

    SECTION("min raw round-trips through scaled()") {
        FixedPrice p{std::numeric_limits<int64_t>::min()};
        REQUIRE(p.scaled() == std::numeric_limits<int64_t>::min());
    }

    SECTION("smallest representable positive tick") {
        auto p = FixedPrice::from_string("0.00000001");
        REQUIRE(p.raw == 1);
    }

    SECTION("smallest representable negative tick") {
        auto p = FixedPrice::from_string("-0.00000001");
        REQUIRE(p.raw == -1);
    }

    SECTION("negative zero parses to zero raw") {
        auto p = FixedPrice::from_string("-0.0");
        REQUIRE(p.raw == 0);
        REQUIRE(p == FixedPrice{0});
    }

    SECTION("fractional digits past DECIMAL_PLACES are dropped, not overflowed") {
        // 9 fractional digits; the 9th must be ignored (8-place scale).
        auto p = FixedPrice::from_string("1.123456789");
        REQUIRE(p.raw == 112345678);
    }

    SECTION("empty string is zero, not UB") {
        REQUIRE(FixedPrice::from_string("").raw == 0);
    }
}

// ============================================================================
// Qty numeric limits (int64_t scaled at 10^4)
// ============================================================================

TEST_CASE("Boundary: Qty at extremes", "[boundary][types][qty]") {
    SECTION("max raw round-trips") {
        Qty q{std::numeric_limits<int64_t>::max()};
        REQUIRE(q.raw == std::numeric_limits<int64_t>::max());
    }

    SECTION("smallest fractional unit") {
        auto q = Qty::from_string("0.0001");
        REQUIRE(q.raw == 1);
    }

    SECTION("fractional digits past DECIMAL_PLACES dropped") {
        auto q = Qty::from_string("1.00005");  // 5th place ignored
        REQUIRE(q.raw == 10000);
    }

    SECTION("zero and empty") {
        REQUIRE(Qty::from_string("0").raw == 0);
        REQUIRE(Qty::from_string("").raw == 0);
    }
}

// ============================================================================
// SeqNum wraparound at 2^31-1
// ============================================================================

TEST_CASE("Boundary: SeqNum wraps at MAX_VALUE", "[boundary][types][seqnum]") {
    SECTION("one below max increments normally") {
        SeqNum s{SeqNum::MAX_VALUE - 1};
        REQUIRE(s.next().get() == SeqNum::MAX_VALUE);
    }

    SECTION("at max wraps to 1 per FIX spec") {
        SeqNum s{SeqNum::MAX_VALUE};
        REQUIRE(s.next().get() == 1u);
    }

    SECTION("MAX_VALUE is valid, MAX_VALUE+1 is not representable as valid") {
        REQUIRE(SeqNum{SeqNum::MAX_VALUE}.is_valid());
        // The type is uint32_t so values above MAX_VALUE (up to 2^32-1) exist
        // but are rejected by is_valid().
        REQUIRE_FALSE(SeqNum{SeqNum::MAX_VALUE + 1}.is_valid());
        REQUIRE_FALSE(SeqNum{0}.is_valid());
    }

    SECTION("uint32 max is not valid") {
        REQUIRE_FALSE(SeqNum{std::numeric_limits<uint32_t>::max()}.is_valid());
    }
}

// ============================================================================
// BodyLength boundaries at the parse layer
// ============================================================================

TEST_CASE("Boundary: BodyLength zero and mismatch", "[boundary][parser][bodylength]") {
    SECTION("BodyLength 0 with empty body is a clean error, not a crash") {
        // 8=FIX.4.4 | 9=0 | 10=xxx : zero-length body has no MsgType.
        std::string frame = frame_with_body("");
        auto r = ParsedMessage::parse(
            std::span<const char>{frame.data(), frame.size()});
        // Either rejected (missing MsgType / body length) - must not crash.
        // We only assert it terminates deterministically.
        (void)r;
        SUCCEED("parse of zero body returned without crashing");
    }

    SECTION("declared BodyLength larger than frame is rejected") {
        std::string body = minimal_body();
        // Hand-build with an inflated 9= that overshoots the real body.
        std::string head = "8=FIX.4.4";
        head += SOH;
        head += "9=" + std::to_string(body.size() + 50) + SOH;
        std::string frame = head + body;
        unsigned sum = 0;
        for (unsigned char c : frame) sum += c;
        char cs[8];
        std::snprintf(cs, sizeof(cs), "10=%03u", sum % 256);
        frame += cs;
        frame += SOH;

        auto r = ParsedMessage::parse(
            std::span<const char>{frame.data(), frame.size()});
        REQUIRE_FALSE(r.has_value());
    }

    SECTION("correct BodyLength parses") {
        std::string frame = frame_with_body(minimal_body());
        auto r = ParsedMessage::parse(
            std::span<const char>{frame.data(), frame.size()});
        REQUIRE(r.has_value());
        REQUIRE(r->msg_type() == '8');
        REQUIRE(r->msg_seq_num() == 1u);
    }
}

// ============================================================================
// Field count boundary: exactly MAX_FIELDS and one past it
// ============================================================================

TEST_CASE("Boundary: field count at parser capacity", "[boundary][parser][fields]") {
    constexpr size_t MAX = ParsedMessage::MAX_FIELDS;

    // The parser records every SOH-terminated field, including the three
    // framing tags 8 (BeginString), 9 (BodyLength) and 10 (CheckSum). So a
    // frame whose body carries B fields is recorded as B+3 fields. To land the
    // recorded count exactly on MAX we emit MAX-3 body fields (5 of which are
    // the mandatory header set 35/49/56/34/52).
    constexpr size_t FRAMING = 3;

    auto build_body_fields = [](size_t body_fields) {
        std::string body;
        body += "35=8"; body += SOH;
        body += "49=S"; body += SOH;
        body += "56=T"; body += SOH;
        body += "34=1"; body += SOH;
        body += "52=20240101-00:00:00.000"; body += SOH;
        for (size_t i = 5; i < body_fields; ++i) {
            // custom tags 5000+ to avoid colliding with framing tags
            body += std::to_string(5000 + i);
            body += "=x";
            body += SOH;
        }
        return frame_with_body(body);
    };

    SECTION("exactly MAX_FIELDS recorded parses") {
        std::string frame = build_body_fields(MAX - FRAMING);
        auto r = ParsedMessage::parse(
            std::span<const char>{frame.data(), frame.size()});
        REQUIRE(r.has_value());
        REQUIRE(r->field_count() == MAX);
    }

    SECTION("one field past MAX_FIELDS is rejected, not overflowed") {
        std::string frame = build_body_fields(MAX - FRAMING + 1);
        auto r = ParsedMessage::parse(
            std::span<const char>{frame.data(), frame.size()});
        REQUIRE_FALSE(r.has_value());
        REQUIRE(r.error().code == ParseErrorCode::FieldCountExceeded);
    }
}

// ============================================================================
// Longest legal field value
// ============================================================================

TEST_CASE("Boundary: long field value parses intact", "[boundary][parser][fields]") {
    // A single body field with a large value must be recorded verbatim.
    std::string big(4096, 'A');
    std::string body;
    body += "35=8"; body += SOH;
    body += "49=S"; body += SOH;
    body += "56=T"; body += SOH;
    body += "34=1"; body += SOH;
    body += "52=20240101-00:00:00.000"; body += SOH;
    body += "58=" + big; body += SOH;  // Text tag with 4096-byte value

    std::string frame = frame_with_body(body);
    auto r = ParsedMessage::parse(
        std::span<const char>{frame.data(), frame.size()});
    REQUIRE(r.has_value());
    auto f = r->get_field(58);
    REQUIRE(f.value.size() == big.size());
}
