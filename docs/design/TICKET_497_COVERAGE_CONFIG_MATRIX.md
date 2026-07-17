# TICKET_497 Phase 4: Coverage / Config Matrix

**Status**: Phase 4 deliverable (boundary + config matrix)
**Created**: 2026-07-16
**Parent**: [TICKET_497](TICKET_497_SQLITE_STYLE_TEST_HARDENING.md)

Which CI job covers which build configuration. The point of writing this down
is to answer two questions the coverage number alone cannot: is there a config
we ship but never exercise, and is there a config the coverage report claims but
no job actually runs. Both are silent gaps.

## Build dimensions

The library is configured along five switches that change which code compiles:

| Switch | Off | On |
|--------|-----|-----|
| `NFX_ENABLE_SIMD` | scalar `build_index_scalar` + scalar SOH/`=` scan | AVX2/AVX-512 paths in `simd_scanner.hpp`, `structural_index.hpp` |
| `NFX_ENABLE_LOGGING` | logging branches compiled out | `logger.hpp` format/emit branches live |
| `NFX_ENABLE_ABSEIL` | std hash containers | Abseil Swiss Tables |
| `NFX_ENABLE_IO_URING` | POSIX socket transport only | io_uring transport paths |
| `NFX_ENABLE_MIMALLOC` | std PMR upstream | mimalloc `SessionHeap` |

Build type (Debug/Release) is a sixth axis: Debug carries the assertion and
`NFX_ASSUME` bodies, Release drops them.

## Job matrix

| Job | OS / compiler | Build | SIMD | Logging | Abseil | io_uring | mimalloc | Runs tests | Sanitizers |
|-----|---------------|-------|------|---------|--------|----------|----------|------------|------------|
| build (GCC) | ubuntu-24.04 / gcc-14 | Release | OFF | OFF | OFF | off | off | yes | none |
| build (Clang) | ubuntu-24.04 / clang-18 | Release | OFF | OFF | OFF | off | off | yes | none |
| build (MSVC) | windows-latest / MSVC | Release | OFF | OFF | OFF | off | off | yes | none |
| build (Apple) | macos-latest / apple-clang | Release | OFF | OFF | OFF | off | off | yes | none |
| coverage (config A) | ubuntu-24.04 / gcc-14 | Debug | OFF | OFF | OFF | off | off | yes | none (gcov) |
| coverage (config B) | ubuntu-24.04 / gcc-14 | Debug | ON (AVX2) | ON | OFF | off | off | yes | none (gcov) |
| clang-tidy | ubuntu-24.04 / clang-18 | Release | OFF | OFF | OFF | off | off | no (static only) | none |
| linux-full | ubuntu-24.04 / gcc-14 | Release | ON | ON | ON | ON | off | yes | none |
| fuzz-smoke | ubuntu-24.04 / clang-18 | Debug | ON (+xsimd) | OFF | OFF | off | off | harnesses only | fuzzer,address,undefined |
| fuzz-nightly | ubuntu-24.04 / clang-18 | Debug | ON (+xsimd) | OFF | OFF | off | off | harnesses only | fuzzer,address,undefined |
| sanitizers | ubuntu-24.04 / gcc-14 | Debug | ON (AVX2) | ON | OFF | off | off | yes (full ctest) | address,undefined |
| mimalloc | ubuntu-24.04 / gcc-14 | Release | ON | OFF | OFF | off | **ON** | yes (full ctest) | none |

## What is covered where

- **SIMD ON**, both AVX2 scanner and structural-index paths: coverage config B
  (measured, AVX2 host), linux-full (built + tested, no coverage capture), and
  the fuzz jobs (address-sanitized, which is how the two OOB bugs in Phase 3
  surfaced). AVX-512 is host-dependent on GitHub runners and stays uncovered in
  the merged tracefile; it is excluded from the gate rather than faked.
- **Logging ON**: coverage config B and linux-full. `logger.hpp` branch
  coverage only exists because config B turns it on; with logging OFF those
  branches compile away entirely.
- **io_uring ON**: linux-full only. Not in any coverage capture, so io_uring
  transport branches do not appear in the branch-coverage number. Documented as
  a known gap, not a silent one.
- **Abseil ON**: linux-full only, and it swaps the hash container implementation
  rather than adding NexusFIX branches, so its coverage impact is upstream code
  that the `--remove '*/_deps/*'` filter drops anyway.
- **Release assertion elision**: every non-coverage job is Release. Coverage is
  Debug (needed for accurate line attribution), so the branch numbers reflect
  the Debug bodies of `NFX_ASSUME`/assert. This is the correct choice for
  measuring reachable branches; it does mean the Release-elided form is only
  exercised for crash-freedom, not coverage.

## Gaps: configs that exist nowhere or run un-sanitized

Two real gaps fall out of the table, both worth naming rather than papering over:

1. **The main test suite never runs under a sanitizer in CI.** ASan/UBSan only
   run against the fuzz harnesses, which build with `NFX_BUILD_TESTS=OFF`. The
   Phase 2 fault-injection suite (`test_fault_injection.cpp`) and the Phase 4
   boundary sweep (`test_boundary_sweep.cpp`) were verified locally under
   `-fsanitize=address,undefined` with `detect_leaks=1`, but nothing in CI holds
   that guard. A `sanitizers` job (Debug, SIMD ON, ASan+UBSan, full `ctest`)
   would close it. Deferred to Phase 5 with the other gates so it lands green,
   not red.

2. **`NFX_ENABLE_MIMALLOC=ON` is in no CI job at all.** The mimalloc
   `SessionHeap` path and its Phase 2 OOM loop (`test_mimalloc.cpp`,
   `NFX_HAS_MIMALLOC` guard) only build and run locally. mimalloc plus ASan is
   incompatible by design (the allocator collides with the ASan interceptor), so
   the honest coverage for that path is a normal-build test job, not a
   sanitized one. No CI job currently enables the switch, so those branches are
   invisible to both the pass/fail gate and the coverage number.

Neither is a correctness regression today; both are places where "CI is green"
covers less than a reader would assume. Phase 5 is where the sanitizer job and
the coverage gate land, so these are recorded here and picked up there rather
than bolted on now.

## Phase 5 update: both gaps closed

Both gaps are closed in Phase 5. The `sanitizers` job runs the full `ctest`
(709/709, SIMD ON, Debug) under `-fsanitize=address,undefined` with
`detect_leaks=1`, so the Phase 2 fault-injection and Phase 4 boundary suites now
hold their sanitizer guard in CI, not just locally. The `mimalloc` job runs the
full suite (726 cases, including the 3 SessionHeap OOM-loop cases and the extra
`mimalloc_tests` binary) with `NFX_ENABLE_MIMALLOC=ON`; it is a plain build
because mimalloc collides with the ASan interceptor. Both are in the job matrix
table above.
