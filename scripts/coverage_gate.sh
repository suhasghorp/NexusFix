#!/usr/bin/env bash
#
# coverage_gate.sh - Hard branch-coverage gate for CI (TICKET_497 Phase 5).
#
# Parses a merged lcov tracefile and fails if overall branch coverage or any
# per-module floor drops below its anchor. Anchors are the coverage measured on
# 2026-07-17 (merged SIMD off + on, GCC 14, lcov 2.0) minus a 2-point margin, so
# the gate lands green and absorbs the run-to-run jitter in branch totals that
# comes from template-instantiation counts shifting between test call sites.
#
# This gate is independent of Codecov: it holds even when the CODECOV_TOKEN
# secret is absent (forks, first-party PRs before the secret is set).
#
# Usage:
#   scripts/coverage_gate.sh <coverage.info>
#
# Exit status: 0 if every floor is met, 1 otherwise (with a per-module report).
#
# Floors are BRANCH percentages. Modules are the top-level directories under
# include/nexusfix/. sbe/ is floored at its measured GCC-artifact ceiling: 169
# of its uncovered branches sit on inlined memcpy/memset size-dispatch lines in
# composite_types.hpp that no source-level test can flip (TICKET_497 Phase 1).
set -euo pipefail

INFO="${1:?usage: coverage_gate.sh <coverage.info>}"

if [[ ! -f "${INFO}" ]]; then
    echo "coverage_gate: tracefile not found: ${INFO}" >&2
    exit 2
fi

# module -> floor (branch %). Overall floor is the special key "__overall__".
# Anchored to the 2026-07-16 merged measurement minus 2 points.
python3 - "${INFO}" <<'PY'
import re, sys, collections

info = sys.argv[1]

# Per-module branch-coverage floors (achieved-minus-2, measured 2026-07-17).
# Achieved snapshot for reference: engine 73.5, memory 69.6, messages 74.6,
# parser 83.1, platform 91.5, sbe 53.2, serializer 92.3, session 86.2,
# store 76.8, transport 73.6, types 75.3, util 71.1; overall 73.0.
# Raised from 2026-07-16 baseline by TICKET_497_3 WS1-WS5 branch coverage work.
FLOORS = {
    "engine":     71.0,
    "memory":     67.0,
    "messages":   72.0,
    "parser":     81.0,
    "platform":   89.0,
    "sbe":        51.0,
    "serializer": 90.0,
    "session":    84.0,
    "store":      74.0,
    "transport":  71.0,
    "types":      73.0,
    "util":       69.0,
}
OVERALL_FLOOR = 71.0

mods = collections.defaultdict(lambda: [0, 0])  # module -> [hit, total]
overall = [0, 0]
cur = None

with open(info) as f:
    for line in f:
        line = line.strip()
        if line.startswith("SF:"):
            m = re.search(r"include/nexusfix/([^/]+)/", line[3:])
            cur = m.group(1) if m else None
        elif line.startswith("BRDA:"):
            taken = line.split(",")[-1]
            hit = 1 if taken not in ("-", "0") else 0
            overall[1] += 1
            overall[0] += hit
            if cur is not None:
                mods[cur][1] += 1
                mods[cur][0] += hit

def rate(h, t):
    return 100.0 * h / t if t else 100.0

failures = []
print(f"{'module':<14} {'branch%':>8} {'hit/total':>12} {'floor':>7}  status")
print("-" * 52)

for mod in sorted(set(list(FLOORS) + list(mods))):
    h, t = mods.get(mod, [0, 0])
    r = rate(h, t)
    floor = FLOORS.get(mod)
    if floor is None:
        status = "(no floor)"
    elif t == 0:
        # A floored module with zero branches means the tracefile lost this
        # module's branch data (path stopped matching, or lcov dropped it).
        # rate() would return a vacuous 100% and green-light the gap, so fail.
        status = "FAIL"
        failures.append(f"{mod}: 0 branches recorded (expected floor {floor:.1f}%)")
    elif r + 1e-9 >= floor:
        status = "ok"
    else:
        status = "FAIL"
        failures.append(f"{mod}: {r:.1f}% < floor {floor:.1f}%")
    fl = f"{floor:.1f}" if floor is not None else "-"
    print(f"{mod:<14} {r:>7.1f}% {h}/{t:<10} {fl:>7}  {status}")

oh, ot = overall
orate = rate(oh, ot)
print("-" * 52)
if ot == 0:
    # No branch data at all: lcov produced no BRDA records (gcov version
    # mismatch, --rc branch flag regression). rate() reads a vacuous 100%.
    ostatus = "FAIL"
    failures.append("overall: 0 branches recorded (no BRDA data in tracefile)")
else:
    ostatus = "ok" if orate + 1e-9 >= OVERALL_FLOOR else "FAIL"
    if ostatus == "FAIL":
        failures.append(f"overall: {orate:.1f}% < floor {OVERALL_FLOOR:.1f}%")
print(f"{'OVERALL':<14} {orate:>7.1f}% {oh}/{ot:<10} {OVERALL_FLOOR:>7.1f}  {ostatus}")

if failures:
    print("\ncoverage_gate: FAILED")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print("\ncoverage_gate: PASSED")
PY
