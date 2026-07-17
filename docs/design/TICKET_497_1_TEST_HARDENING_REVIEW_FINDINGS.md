# TICKET_497_1: Post-Implementation Review Findings for Test Hardening (TICKET_497)

| Field | Value |
|-------|-------|
| Created | 2026-07-16 |
| Updated | 2026-07-16 |
| Status | Open |
| Priority | High |
| Category | Testing / CI / Parser Safety |
| Source | Code review of commit 114b0d0 (TICKET_497) |

---

## Summary

Review of commit 114b0d0 ("Add SQLite-style test hardening") before push. The
test content is solid: fault injection is deterministic, the property test uses
a fixed seed, all new test files are registered, and three of the four
fuzz-found parser fixes are correct (SIMD scanner OOB bounds, equals_positions
overflow guard, CheckSum field-loop stop, tag/int digit-accumulation guards).

The review found one incomplete UB fix in production code and two defects that
make the new CI jobs fail on every GitHub run, plus several medium/low issues.

---

## Findings

### HIGH-1: FixedPrice/Qty::from_string overflow fix is incomplete (UB remains)

`include/nexusfix/types/field_types.hpp:152` (FixedPrice) and the matching
guard in `Qty::from_string` only ensure `integer_part * SCALE <= INT64_MAX`.
The final combine at `field_types.hpp:167`:

```cpp
int64_t result = integer_part * SCALE + fractional_part;
```

adds `fractional_part` (up to `SCALE - 1`) on top, which can still overflow.

**Reproduction** (verified with `-fsanitize=signed-integer-overflow`):

```
FixedPrice::from_string("92233720368.99999999")
  -> field_types.hpp:167: runtime error: signed integer overflow:
     99999999 + 9223372036800000000 cannot be represented in type 'long int'

Qty::from_string("922337203685477.9999")   // same structure, same bug
```

Analysis: the guard admits `integer_part` up to `INT64_MAX / SCALE`
(92233720368 for SCALE=1e8). Then `integer_part * SCALE = 9223372036800000000`
leaves headroom of only 54775807, while `fractional_part` can reach 99999999.

**Fix**: tighten the guard so the fractional headroom is reserved, e.g.

```cpp
if (integer_part > ((std::numeric_limits<int64_t>::max() - (SCALE - 1)) / SCALE - digit) / 10)
```

or check the final addition explicitly. Per the regression-test rule, add a
failing-first case to `tests/test_fuzz_regressions.cpp` plus minimized inputs
under `fuzz/corpus/fuzz_serializer_roundtrip/regressions/`.

### HIGH-2: scripts/coverage_gate.sh is not committed; coverage job fails every run

`.github/workflows/ci.yml:230` runs `scripts/coverage_gate.sh coverage.info`,
but `git ls-tree 114b0d0 scripts/` contains only `check_perf_regression.sh`.
Root cause: `.gitignore:106` ignores `scripts/` wholesale; the gate script
exists only in the local working tree. On GitHub the step exits 127 and the
coverage job goes red.

**Fix**: `git add -f scripts/coverage_gate.sh`.

### HIGH-3: Both fuzz CI jobs cannot compile: clang-18 + libstdc++ has no std::expected

`fuzz/CMakeLists.txt` forces `-stdlib=libstdc++`, but libstdc++'s `<expected>`
is gated on `__cpp_concepts >= 202002L` and clang-18 (the CI compiler) defines
201907L. Verified: configuring the exact fuzz-smoke cmake line with clang-18
fails with `no template named 'expected' in namespace 'std'`. Local runs
succeeded because the local clang is 22. As committed, `fuzz-smoke` fails
every PR and `fuzz-nightly` fails every night at the build step, so nothing
ever fuzzes in CI.

Same defect class as TICKET_492/493/494 (clang-tidy stdlib mismatch). The
clang-tidy resolution (libc++) conflicts with the packaged libFuzzer runtime
per the fuzz CMake comment, so the fix likely needs a newer clang in the fuzz
jobs.

### MED-1: Nightly fuzz corpus cache never accumulates past the first run

`fuzz-nightly` uses `actions/cache@v4` keyed on
`fuzz-corpus-${{ hashFiles('fuzz/corpus/**') }}`. The seed hash is constant, and
actions/cache skips saving on an exact primary-key hit, so night 2+ restores
night 1's corpus and discards its own discoveries.

**Fix**: primary key includes `${{ github.run_id }}`, seed hash becomes a
restore-key prefix.

### MED-2: coverage_gate.sh passes vacuously on empty/broken tracefiles

`rate()` returns 100.0 when the branch total is 0, per module and overall. If
lcov silently drops branch data (gcov version mismatch, rc flag regression) or
a module path stops matching, the gate green-lights it.

**Fix**: fail when the overall branch total is 0 or a floored module has 0
branches.

### MED-3: Nightly cron triggers the entire workflow, not just fuzz-nightly

`on.schedule` is workflow-wide; only `fuzz-smoke` opts out via
`if: github.event_name != 'schedule'`. Build, coverage+gate, clang-tidy,
sanitizers, mimalloc, and the performance-regression job all run at 03:17 UTC
nightly. Wasted runners plus nightly risk of a spurious perf-gate failure on an
idle default branch.

**Fix**: gate non-fuzz jobs on `github.event_name != 'schedule'` (or move
fuzz-nightly to its own workflow file).

### LOW-1: TICKET_497 design docs not committed

`docs/design/TICKET_497_SQLITE_STYLE_TEST_HARDENING.md` and
`TICKET_497_COVERAGE_CONFIG_MATRIX.md` are caught by `.gitignore` `**/TICKET_*.md`
and are untracked, while ci.yml comments reference them. Other design docs were
force-added; these were missed. **Fix**: `git add -f` both (and this file).

### LOW-2: Smoke runs write mutated inputs into the checked-in seed corpus

Both the CI smoke loop and the cmake `fuzz_smoke` target pass `fuzz/corpus/<h>`
as the first (writable) corpus dir; a local run dumps hundreds of hash-named
untracked files into the repo. The nightly job's scratch-dir-first pattern is
correct; smoke should match.

### LOW-3: Silent truncation semantics inconsistent across parsers

On overflow, `FixedPrice/Qty::from_string` and `StructuralFieldView::get_int`
`break` and return a truncated (wrong) value, while `FieldView::as_int/as_uint`
return `std::nullopt`. For a price parser, a silently wrong value is worse than
a rejection. Consider unifying on reject once HIGH-1 is fixed.

### LOW-4: No parse benchmark before/after for the hot-path guards

The per-digit overflow guards add a compare plus a divide-by-constant-10
(compiled to multiply/shift) in the hottest digit loops (tag parse, get_int,
as_int). Cost is likely small but unmeasured against the <200ns
ExecutionReport target. Run the standard before/after parse benchmark and
record it under `docs/compare/`.

### LOW-5: Minor cleanups

- `-DNFX_FUZZ_SMOKE_SECONDS=60` in the fuzz-smoke configure is dead; the run
  step hardcodes `-max_total_time=60`.
- `tests/test_fuzz_regressions.cpp` hardcodes `256` instead of referencing
  `ParsedMessage::MAX_FIELDS` (test_boundary_sweep.cpp does it correctly).

---

## Verified As Correct

- SIMD scanner bound fix (`i + width <= size`) in all six scanners: correct,
  the scalar epilogue covers the tail.
- `equals_positions` guard in scalar builder and all three SIMD tails: memory
  safe; a legitimate 256-field message still records all fields.
- CheckSum (tag 10) loop stop in `ParsedMessage::parse` and `IndexedParser`:
  checksum validation still runs; trailing bytes are framing concerns.
- All other digit-accumulation guards (`tag_at`, `FieldView::as_int/as_uint`,
  `FieldIterator`, runtime tag loop, `ranges_utils`): bound math correct.
- Fault-injection suite deterministic (counting FailingResource, no timing
  dependence); socket fault tests passed 30/30 repeat runs.
- Property test uses fixed seed 20240716, 500 trials.
- All four fuzz harnesses handle size-0 input; regression tests hardcode
  inputs in-source (no CWD dependence); new test files registered in
  `tests/CMakeLists.txt`.

---

## Execution Order

1. HIGH-1: fix FixedPrice/Qty overflow + failing-first regression test
2. HIGH-2: force-add coverage_gate.sh (with MED-2 hardening)
3. HIGH-3: fuzz job toolchain fix
4. MED-1, MED-3: CI workflow corrections
5. LOW items opportunistically with the above
