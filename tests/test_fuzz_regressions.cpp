// test_fuzz_regressions.cpp
//
// Permanent regression tests for crashes found by the TICKET_497 Phase 3 fuzz
// harnesses. Policy (TICKET_497): every fuzzer-found crash becomes a checked-in
// regression case that fails before the fix. These run in the main suite so the
// ASan/UBSan CI job guards them even when the fuzzers are not built.
//
// The raw crashing inputs also live under fuzz/corpus/<harness>/regressions/
// for the fuzz build; these tests are the minimized, deterministic reproducers.

#include <catch2/catch_test_macros.hpp>

#include "nexusfix/parser/structural_index.hpp"
#include "nexusfix/parser/simd_scanner.hpp"
#include "nexusfix/parser/runtime_parser.hpp"

#include <span>
#include <string>
#include <vector>
#include <cstdint>
#include <cstdio>
#include <limits>

// ============================================================================
// Bug 1: equals_positions stack-buffer-overflow on '='-heavy input
// ============================================================================
//
// build_index_scalar (and the SIMD tail loops) bounded only soh_count in the
// scan loop. An input with far more '=' than SOH drove equals_count past
// MAX_FIELDS and wrote off the end of equals_positions. Original fuzz input:
// 266 '=' and 1 SOH. Reproducer: any buffer with > MAX_FIELDS '=' characters.

TEST_CASE("Fuzz regression: '='-heavy input does not overflow equals_positions",
          "[fuzz][regression][parser]") {
    // MAX_FIELDS is 256; use comfortably more '=' than that, with a single SOH
    // so soh_count stays low and never trips the old (soh-only) loop bound.
    std::string input(300, '=');
    input.push_back('\x01');

    std::span<const char> bytes{input.data(), input.size()};

    // Must not read/write out of bounds (validated under ASan). The recorded
    // equals_count must be clamped to the array capacity.
    const auto idx = nfx::simd::build_index_scalar(bytes);
    REQUIRE(idx.equals_count <= 256);
    REQUIRE(idx.soh_count <= 256);

    // Walking every recorded field stays in bounds.
    for (std::size_t i = 0; i < idx.field_count(); ++i) {
        (void)idx.tag_at(bytes, i);
        (void)idx.value_at(bytes, i);
    }

    // The runtime parser sits on top of the same index; it must reject cleanly.
    auto result = nfx::ParsedMessage::parse(bytes);
    (void)result;  // value or error, never a crash
}

TEST_CASE("Fuzz regression: '='-heavy input via runtime dispatch (SIMD tail)",
          "[fuzz][regression][parser]") {
    // Force through build_index() so the SIMD tail loop (which had the same
    // equals_count bug) is exercised when SIMD is compiled in.
    std::string input(400, '=');
    input.push_back('\x01');
    std::span<const char> bytes{input.data(), input.size()};

    const auto idx = nfx::simd::build_index(bytes);
    REQUIRE(idx.equals_count <= 256);
}

// ============================================================================
// Bug 2: SIMD scanner aligned-load OOB on unpadded, non-32-multiple buffer
// ============================================================================
//
// find_equals_xsimd / find_soh_xsimd aligned the read pointer with a scalar
// prologue, then bounded the aligned-load loop on (size & ~(width-1)). Because
// the prologue shifts i forward, the last aligned load could start below that
// bound yet read past the end of an unpadded buffer whose length is not a
// multiple of the register width. Original fuzz input: a 142-byte heap buffer.
//
// Reproducer: scan a heap buffer sized to a non-multiple of 64 (so both AVX2
// and AVX-512 tails are stressed) with no trailing padding, containing no match
// so the loop runs to the very end. Run several odd sizes and start offsets so
// the alignment prologue lands at different shifts regardless of malloc's base
// alignment.

TEST_CASE("Fuzz regression: SIMD scanner does not read past unpadded buffer",
          "[fuzz][regression][parser][simd]") {
    for (std::size_t len : {65u, 97u, 129u, 142u, 191u, 255u}) {
        // Heap allocation: ASan poisons the byte immediately after it.
        std::vector<char> buf(len, 'A');  // no SOH, no '=' -> scan to the end

        std::span<const char> bytes{buf.data(), buf.size()};

        for (std::size_t start = 0; start < 4 && start < len; ++start) {
            // No match: both scanners must walk to data.size() without OOB.
            REQUIRE(nfx::simd::find_equals(bytes, start) == bytes.size());
            REQUIRE(nfx::simd::find_soh(bytes, start) == bytes.size());
        }

        // A full parse also exercises the scanner on the same unpadded buffer.
        auto result = nfx::ParsedMessage::parse(bytes);
        (void)result;
    }
}

// ============================================================================
// Bug 3: signed-integer-overflow UB in the digit-accumulation parsers
// ============================================================================
//
// tag_at / as_int / as_uint / FieldIterator::next / the runtime tag loop / the
// FixedPrice+Qty from_string integer part all accumulated digits as
// `acc = acc*10 + digit` with no bound. An overlong all-digit tag or field
// value off the wire overflowed a signed int/int64_t, which is UB (UBSan
// aborts). Fix: reject/stop before the overflowing multiply-add. The fuzzers
// hit this in under a second on every harness. Reproducers assert the guarded
// result (invalid sentinel / clean stop), which under UBSan also proves no UB.

TEST_CASE("Fuzz regression: overlong tag number does not overflow (int)",
          "[fuzz][regression][parser]") {
    // A tag made of 40 nines is far past INT_MAX. Must be rejected, not wrapped.
    std::string tag_digits(40, '9');
    std::string input = tag_digits + "=X\x01";
    std::span<const char> bytes{input.data(), input.size()};

    // FieldIterator: overflowing tag -> invalid field + InvalidTagNumber.
    nfx::FieldIterator iter{bytes};
    nfx::FieldView f = iter.next();
    REQUIRE_FALSE(f.is_valid());
    REQUIRE(iter.last_error() == nfx::ParseErrorCode::InvalidTagNumber);

    // Structural index tag_at: overflowing tag -> 0 sentinel, no UB.
    const auto idx = nfx::simd::build_index(bytes);
    for (std::size_t i = 0; i < idx.field_count(); ++i) {
        (void)idx.tag_at(bytes, i);  // must not overflow
    }

    // Runtime parse must reject cleanly (value or error, never a crash/UB).
    auto result = nfx::ParsedMessage::parse(bytes);
    (void)result;
}

TEST_CASE("Fuzz regression: overlong integer field value does not overflow",
          "[fuzz][regression][parser]") {
    // 30 nines overflows both int64 (as_int) and uint64 (as_uint).
    std::string big(30, '9');
    nfx::FieldView f{34, std::span<const char>{big.data(), big.size()}};

    // Overflow -> nullopt, not a wrapped value and not UB.
    REQUIRE_FALSE(f.as_int().has_value());
    REQUIRE_FALSE(f.as_uint().has_value());

    // A value that fits must still parse.
    std::string ok = "12345";
    nfx::FieldView g{34, std::span<const char>{ok.data(), ok.size()}};
    REQUIRE(g.as_int() == 12345);
    REQUIRE(g.as_uint() == 12345u);
}

// ============================================================================
// Bug 4: parser recorded fields after the CheckSum (tag 10)
// ============================================================================
//
// The field-recording loop walked every SOH-delimited field in the buffer and
// never stopped at CheckSum (tag 10), even though validate_checksum treats the
// first "\x0110=" as the message boundary. A valid message with trailing bytes
// after 10= got those bytes recorded as extra fields. Because parse_header is
// first-wins and stops at the body while the recorded field set kept the
// trailing duplicates, re-emitting the recorded fields and reparsing produced a
// different header (the fuzzer hit a duplicate 56= where the reparse resolved
// TargetCompID to the trailing occurrence). Fix: stop the field loop at tag 10.

TEST_CASE("Fuzz regression: bytes after CheckSum are not recorded (idempotent)",
          "[fuzz][regression][parser]") {
    constexpr char SOH = '\x01';
    // A well-formed message with a correct checksum, then trailing "56=EVIL".
    std::string body =
        std::string("35=A") + SOH + "49=SENDER" + SOH + "56=TARGET" + SOH +
        "34=1" + SOH + "52=20231215-10:30:00" + SOH + "98=0" + SOH + "108=30" + SOH;
    std::string frame = std::string("8=FIX.4.4") + SOH +
                        "9=" + std::to_string(body.size()) + SOH + body;
    unsigned sum = 0;
    for (unsigned char c : frame) sum += c;
    char cs[8];
    std::snprintf(cs, sizeof(cs), "10=%03u", sum % 256);
    std::string valid = frame + cs + SOH;
    std::string with_trailing = valid + "56=EVIL" + SOH;

    auto parsed = nfx::ParsedMessage::parse(
        std::span<const char>{with_trailing.data(), with_trailing.size()});
    REQUIRE(parsed.has_value());

    // The trailing 56=EVIL must not be recorded: TargetCompID stays TARGET, and
    // the last recorded field is the CheckSum, not the trailing garbage.
    REQUIRE(parsed->target_comp_id() == "TARGET");
    const std::size_t n = parsed->field_count();
    REQUIRE(n >= 1);
    REQUIRE(parsed->field_at_safe(n - 1).tag == 10);
    for (std::size_t i = 0; i < n; ++i) {
        // No field carries the trailing sentinel value.
        REQUIRE(parsed->field_at_safe(i).as_string() != "EVIL");
    }
}
