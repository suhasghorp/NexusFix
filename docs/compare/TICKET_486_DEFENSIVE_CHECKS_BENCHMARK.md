# TICKET_486: Defensive Checks Overhead Benchmark

**Date**: 2026-06-19
**System**: Linux, GCC 14.2.0, -O3 -march=native
**CPU**: 3.418 GHz
**Iterations**: 100,000 (best of 3 runs)
**Benchmark**: `benchmarks/defensive_checks_bench.cpp`

---

## Background

TICKET_486 adds QuickFIX-parity defensive checks to `SessionManager::on_data_received()`:

1. **CompID validation** (QuickFIX `isCorrectCompID()`): Verify SenderCompID/TargetCompID match session config
2. **SendingTime accuracy** (QuickFIX `isGoodTime()`): Reject messages with stale timestamps
3. **PossDupFlag+OrigSendingTime** (QuickFIX `doPossDup()`): Reject PossDup=Y without OrigSendingTime

All checks are configurable via `SessionConfig` (`validate_comp_ids`, `check_latency`).

---

## Full Session Path Overhead

Latency of `on_data_received()` processing a valid heartbeat through a fully established session:

| Configuration | Mean | P50 | P99 | Overhead |
|---------------|------|-----|-----|----------|
| Baseline (no checks) | 158.57 ns | 158.01 ns | 165.32 ns | --- |
| CompID validation only | 171.33 ns | 169.71 ns | 184.93 ns | +12.77 ns (+8.1%) |
| SendingTime accuracy only | 175.13 ns | 174.10 ns | 181.71 ns | +16.56 ns (+10.4%) |
| All checks enabled | 183.09 ns | 181.71 ns | 198.97 ns | +24.53 ns (+15.5%) |

---

## Isolated Check Microbenchmarks

Cost of each check in isolation (no session infrastructure overhead):

| Check | Mean | P50 | Notes |
|-------|------|-----|-------|
| CompID comparison (2x `string_view` ==) | 12.02 ns | 12.00 ns | Two 6-char comparisons |
| SendingTime parse + `system_clock::now()` | 26.34 ns | 26.63 ns | Date parsing + syscall |
| `system_clock::now()` alone | 24.04 ns | 23.99 ns | vDSO clock_gettime |

The SendingTime check cost is **91% `system_clock::now()`** (24ns of 26ns). The FIX timestamp parsing itself costs ~2ns.

---

## Analysis

CompID validation adds ~12ns per message. This is two `string_view` comparisons with 6-character CompIDs. For longer CompIDs the cost scales linearly but remains under 20ns for typical 8-16 character IDs.

SendingTime accuracy adds ~17-26ns per message. The cost is dominated by `system_clock::now()` (a vDSO call to `clock_gettime`). The timestamp parsing (year/month/day/hour/min/sec extraction + epoch conversion) itself is ~2ns. This is the same cost QuickFIX pays in its `isGoodTime()` check.

PossDupFlag validation adds ~0ns. It's a single boolean check on a pre-parsed header field. The cost is below RDTSC measurement resolution.

---

## Production Guidance

| Use Case | Recommended Config | Latency |
|----------|-------------------|---------|
| Ultra-low-latency (HFT) | `check_latency=false`, `validate_comp_ids=false` | ~158 ns |
| Low-latency with safety | `check_latency=false`, `validate_comp_ids=true` | ~171 ns |
| Standard (FIX compliant) | `check_latency=true`, `validate_comp_ids=true` | ~183 ns |

For HFT systems where the counterparty is trusted and timestamps are validated at the gateway level, disable both checks for minimum latency. For standard FIX connectivity, enable both checks (the 25ns overhead is negligible compared to network RTT).

---

## Comparison with QuickFIX

QuickFIX's `Session::verify()` performs these same checks on every message, plus DataDictionary validation. NexusFIX makes them individually configurable.

| Check | QuickFIX | NexusFIX | Configurable |
|-------|----------|----------|--------------|
| CompID validation | Always on | 12 ns, `validate_comp_ids` flag | Yes |
| SendingTime accuracy | Always on | 17 ns, `check_latency` flag | Yes |
| DataDictionary validation | Always on | Not implemented (schema-agnostic) | N/A |
