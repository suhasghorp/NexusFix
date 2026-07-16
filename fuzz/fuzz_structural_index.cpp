// fuzz_structural_index.cpp
//
// libFuzzer harness: raw counterparty bytes -> FIXStructuralIndex.
//
// The structural index is the first thing that touches untrusted wire bytes:
// it scans for SOH delimiters and records field boundaries before any semantic
// parse. It must never read out of bounds or crash on any input, however
// malformed. Exercise the scalar builder directly (the SIMD dispatch is a
// performance variant of the same contract; NFX_ENABLE_SIMD selects it in the
// build matrix, and both must survive the same corpus).
//
// Build: clang++ -std=c++23 -fsanitize=fuzzer,address,undefined ...

#include "nexusfix/parser/structural_index.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const char> bytes{reinterpret_cast<const char*>(data), size};

    // Scalar path always available. When SIMD is compiled in, build_index()
    // dispatches to AVX2/AVX-512; run it too so the vectorized boundary
    // handling gets fuzzed on the same input.
    const nfx::simd::FIXStructuralIndex idx = nfx::simd::build_index_scalar(bytes);

    // Walk every recorded field. Accessors must clamp to the buffer even when
    // the index was built over a truncated or garbage frame.
    const std::size_t n = idx.field_count();
    for (std::size_t i = 0; i < n; ++i) {
        (void)idx.tag_at(bytes, i);
        (void)idx.value_at(bytes, i);
        (void)idx.field_bounds(i);
    }

    // Out-of-range access must be guarded, not read past the end.
    (void)idx.tag_at(bytes, n);
    (void)idx.value_at(bytes, n + 7);

    // find_tag on an arbitrary tag must terminate cleanly whether found or not.
    (void)idx.find_tag(bytes, 35);
    (void)idx.find_tag(bytes, -1);

#if defined(NFX_HAS_SIMD)
    const nfx::simd::FIXStructuralIndex idx_simd = nfx::simd::build_index(bytes);
    const std::size_t m = idx_simd.field_count();
    for (std::size_t i = 0; i < m; ++i) {
        (void)idx_simd.tag_at(bytes, i);
        (void)idx_simd.value_at(bytes, i);
    }
    (void)idx_simd.tag_at(bytes, m);
#endif

    return 0;
}
