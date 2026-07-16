// fuzz_runtime_parser.cpp
//
// libFuzzer harness: raw bytes -> ParsedMessage / IndexedParser.
//
// One layer above the structural index: semantic parse of header + fields,
// body-length validation, duplicate-tag handling. Every malformed frame must
// resolve to a std::expected error, never a crash. On a successful parse, walk
// the recovered fields so accessor read paths are fuzzed against arbitrary
// (attacker-chosen) field content.
//
// Build: clang++ -std=c++23 -fsanitize=fuzzer,address,undefined ...

#include "nexusfix/parser/runtime_parser.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const char> bytes{reinterpret_cast<const char*>(data), size};

    // Single-shot semantic parse.
    if (auto msg = nfx::ParsedMessage::parse(bytes)) {
        (void)msg->msg_type();
        (void)msg->msg_seq_num();
        (void)msg->sender_comp_id();
        (void)msg->target_comp_id();
        const std::size_t n = msg->field_count();
        for (std::size_t i = 0; i < n; ++i) {
            const auto& f = msg->field_at_safe(i);
            (void)f.tag;
            (void)f.value;
        }
        (void)msg->get_field(35);
        (void)msg->has_field(10);
    } else {
        (void)msg.error().code;
    }

    // Indexed (hash-table) parse: permissive and strict both feed the same
    // dedup/repeating-group logic. Strict rejects duplicate non-repeating tags,
    // a distinct branch set worth fuzzing.
    if (auto ip = nfx::IndexedParser::parse(bytes)) {
        (void)ip->get_field(35);
        (void)ip->has_field(55);
        (void)ip->get_int(34);
        (void)ip->get_string(49);
    }
    if (auto sp = nfx::StrictIndexedParser::parse(bytes)) {
        (void)sp->has_field(35);
    }

    // Streaming framer: fuzz message-boundary detection over a raw byte run.
    nfx::StreamParser stream;
    const std::size_t consumed = stream.feed(bytes);
    (void)consumed;
    while (stream.has_message()) {
        auto [start, end] = stream.next_message();
        (void)start;
        (void)end;
    }

    return 0;
}
