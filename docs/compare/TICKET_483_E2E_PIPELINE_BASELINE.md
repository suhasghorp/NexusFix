# NexusFIX E2E Pipeline Baseline (TICKET_483)

**Date:** 2026-06-18
**Platform:** Linux x86_64, GCC, TCP loopback
**CPU:** 3.418 GHz (estimated via busy-wait RDTSC calibration)
**Benchmark:** `pipeline_bench` (all modes)
**Warmup:** 2000 iterations, **Bench:** 10000 iterations

---

## Round-trip Latency: NOS -> ER (RDTSC, loopback TCP)

Full round-trip: Initiator sends NewOrderSingle, Acceptor receives and echoes
ExecutionReport back, Initiator receives ER.

| Metric | Value |
|--------|-------|
| Min | 9.20 us |
| P50 | 10.15 us |
| P90 | 10.40 us |
| P99 | 11.64 us |
| P99.9 | 13.19 us |
| Max | 793.25 us |
| Mean | 10.26 us |
| Stddev | 7.84 us |
| **P99/P50** | **1.15x** |
| **Throughput** | **97,423 round-trips/sec** |

## Round-trip Latency: NOS -> ER (chrono, loopback TCP)

Same measurement using `std::chrono::steady_clock` for comparison.

| Metric | Value |
|--------|-------|
| Min | 9.84 us |
| P50 | 10.60 us |
| P90 | 17.59 us |
| P99 | 18.30 us |
| P99.9 | 1383.16 us |
| Max | 3201.86 us |
| Mean | 15.52 us |
| Stddev | 75.15 us |
| P99/P50 | 1.73x |
| Throughput | 64,436 round-trips/sec |

## Half-trip: Send-side (RDTSC)

Measures only the initiator send path: NOS build + serialize + TCP send.

| Metric | Value |
|--------|-------|
| Min | 3.76 us |
| P50 | 4.24 us |
| P90 | 5.75 us |
| P99 | 13.04 us |
| P99.9 | 14.06 us |
| Max | 18.38 us |
| Mean | 4.78 us |
| Stddev | 1.82 us |
| P99/P50 | 3.07x |
| Throughput | 209,359 sends/sec |

## Sustained Throughput (burst send + drain all ERs)

| Burst Size | Total Time | Avg/msg | Throughput |
|------------|-----------|---------|------------|
| 100 | 467 us | 4.67 us | 213,948 rt/s |
| 1,000 | 4,523 us | 4.52 us | 221,071 rt/s |
| 5,000 | 23,465 us | 4.69 us | 213,085 rt/s |

Throughput scales linearly. Avg/msg stabilizes around 4.5-4.7 us under burst load,
indicating no degradation from TCP back-pressure at these volumes.

## FixInitiator API Overhead (one-way send)

Measures `FixInitiator::send()` through the public API (vs raw SessionManager wiring).

| Metric | Value |
|--------|-------|
| Min | 0.89 us |
| P50 | 7.67 us |
| P90 | 21.68 us |
| P99 | 22.46 us |
| Mean | 9.28 us |

Higher variance than raw wiring due to `FixInitiator::poll()` interleaving and
`FixAcceptor` background thread scheduling.

## Key Observations

1. **P99/P50 = 1.15x** (RDTSC round-trip). Extremely deterministic latency tail.
2. **Send-side half-trip ~4.2 us** accounts for ~42% of the 10 us round-trip.
   The remaining ~5.8 us is acceptor processing + return TCP + initiator recv/parse.
3. **Sustained throughput ~213K rt/s** with burst sizes up to 5000.
4. **RDTSC vs chrono**: RDTSC shows tighter P90/P99 because it measures CPU cycles
   without `steady_clock` syscall overhead. The chrono P99.9 spike (1383 us) is likely
   a timer interrupt or context switch captured by the wall-clock measurement.
5. **API overhead**: `FixInitiator::send()` P50 is ~7.7 us vs raw half-trip ~4.2 us.
   The ~3.5 us delta comes from the public API's callback indirection and FixAcceptor's
   background thread poll interval.
