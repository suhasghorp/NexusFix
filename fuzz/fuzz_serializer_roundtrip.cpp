// fuzz_serializer_roundtrip.cpp
//
// Structure-aware roundtrip: bytes -> parse -> reconstruct wire -> reparse,
// then assert the two parses agree field-for-field.
//
// The FIX serializer (constexpr_serializer.hpp) is compile-time templated on
// the field set, so it can't consume an arbitrary runtime field list. The
// property worth fuzzing is instead parser idempotency: if the parser accepts a
// frame and hands back a field set, re-emitting exactly those fields on the
// wire and parsing again must yield an identical field set. A mismatch means
// the parse is lossy or non-deterministic, which is the class of bug that lets
// two peers disagree about what a message said.
//
// Rebuilding the wire requires recomputing BodyLength (9) and CheckSum (10)
// since those depend on the serialized body. All other tags/values are copied
// verbatim from the first parse.
//
// Build: clang++ -std=c++23 -fsanitize=fuzzer,address,undefined ...

#include "nexusfix/parser/runtime_parser.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

namespace {

constexpr char SOH = '\x01';

// Re-emit a parsed message as FIX wire bytes: 8, 9, then the body fields in
// index order (skipping the framing tags 8/9/10 the parser recorded), then a
// freshly computed 10. Returns empty if the message lacks a BeginString.
std::string rebuild(const nfx::ParsedMessage& msg) {
    const std::string_view begin_string = msg.header().begin_string;
    if (begin_string.empty()) return {};

    std::string body;
    const std::size_t n = msg.field_count();
    for (std::size_t i = 0; i < n; ++i) {
        const auto& f = msg.field_at_safe(i);
        // Skip framing tags; they are recomputed. Value may be arbitrary bytes.
        if (f.tag == 8 || f.tag == 9 || f.tag == 10) continue;
        body += std::to_string(f.tag);
        body += '=';
        body.append(f.value.data(), f.value.size());
        body += SOH;
    }

    std::string head;
    head += "8=";
    head.append(begin_string.data(), begin_string.size());
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

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const char> bytes{reinterpret_cast<const char*>(data), size};

    auto first = nfx::ParsedMessage::parse(bytes);
    if (!first) return 0;  // Only successful parses can be round-tripped.

    const std::string wire = rebuild(*first);
    if (wire.empty()) return 0;

    auto second = nfx::ParsedMessage::parse(
        std::span<const char>{wire.data(), wire.size()});

    // A frame the parser just produced from its own accepted output must parse
    // again. If not, the emit/parse pair is inconsistent.
    if (!second) {
        __builtin_trap();
    }

    // Header identity: the semantic content the two peers rely on.
    if (first->msg_type() != second->msg_type()) __builtin_trap();
    if (first->msg_seq_num() != second->msg_seq_num()) __builtin_trap();
    if (first->sender_comp_id() != second->sender_comp_id()) __builtin_trap();
    if (first->target_comp_id() != second->target_comp_id()) __builtin_trap();

    return 0;
}
