#!/usr/bin/env python3
"""Generate seed corpus for TICKET_497 fuzz harnesses.

Writes one file per seed under fuzz/corpus/<harness>/. Seeds are real FIX
frames (correct BodyLength/CheckSum) across 4.2/4.3/4.4/5.0 plus a handful of
known-malformed frames lifted from the parser's negative tests. libFuzzer
mutates from these; good seeds cut the time to reach deep branches.

Run from repo root:  python3 fuzz/gen_corpus.py
Idempotent: overwrites the generated seeds, leaves corpus/<h>/regressions alone.
"""
import os

SOH = "\x01"
HARNESSES = [
    "fuzz_structural_index",
    "fuzz_runtime_parser",
    "fuzz_session_input",
    "fuzz_serializer_roundtrip",
]


def frame(begin_string, body_fields):
    body = "".join(f"{t}={v}{SOH}" for t, v in body_fields)
    head = f"8={begin_string}{SOH}9={len(body)}{SOH}"
    partial = head + body
    checksum = sum(ord(c) for c in partial) % 256
    return partial + f"10={checksum:03d}{SOH}"


def nos(begin_string, seq):
    # NewOrderSingle (35=D)
    return frame(begin_string, [
        ("35", "D"), ("49", "SENDER"), ("56", "TARGET"), ("34", str(seq)),
        ("52", "20231215-10:30:00"), ("11", "ORDER123"), ("55", "AAPL"),
        ("54", "1"), ("60", "20231215-10:30:00"), ("38", "100"),
        ("40", "2"), ("44", "150.25"),
    ])


def exec_report(begin_string, seq):
    # ExecutionReport (35=8)
    return frame(begin_string, [
        ("35", "8"), ("49", "SENDER"), ("56", "TARGET"), ("34", str(seq)),
        ("52", "20231215-10:30:00"), ("37", "ORDER123"), ("17", "EXEC1"),
        ("150", "0"), ("39", "0"), ("55", "AAPL"), ("54", "1"),
        ("38", "100"), ("14", "0"), ("6", "0"), ("44", "150.25"),
    ])


def logon(begin_string, seq):
    # Logon (35=A) - counterparty view (49=TARGET) for the session harness
    return frame(begin_string, [
        ("35", "A"), ("49", "TARGET"), ("56", "SENDER"), ("34", str(seq)),
        ("52", "20231215-10:30:00"), ("98", "0"), ("108", "30"),
    ])


def heartbeat(begin_string, seq):
    return frame(begin_string, [
        ("35", "0"), ("49", "TARGET"), ("56", "SENDER"), ("34", str(seq)),
        ("52", "20231215-10:30:00"),
    ])


def resend_request(begin_string, seq):
    return frame(begin_string, [
        ("35", "2"), ("49", "TARGET"), ("56", "SENDER"), ("34", str(seq)),
        ("52", "20231215-10:30:00"), ("7", "1"), ("16", "0"),
    ])


BEGIN = {
    "42": "FIX.4.2",
    "43": "FIX.4.3",
    "44": "FIX.4.4",
    "50": "FIXT.1.1",  # 5.0 uses the FIXT.1.1 session layer
}

# Known-malformed frames (hand-enumerated negatives; fuzzer beats hand-enum,
# but these prime the error branches immediately).
MALFORMED = {
    "empty": "",
    "no_soh": "8=FIX.4.4 9=10 35=D",
    "bad_bodylen": "8=FIX.4.4" + SOH + "9=999" + SOH + "35=D" + SOH + "10=000" + SOH,
    "missing_checksum": "8=FIX.4.4" + SOH + "9=5" + SOH + "35=D" + SOH,
    "non_digit_tag": "8=FIX.4.4" + SOH + "9=5" + SOH + "3X=D" + SOH + "10=000" + SOH,
    "unterminated_field": "8=FIX.4.4" + SOH + "9=5" + SOH + "35=D",
    "only_soh": SOH * 8,
    "huge_bodylen": "8=FIX.4.4" + SOH + "9=2147483647" + SOH + "35=D" + SOH,
}


def write(harness, name, data):
    d = os.path.join(REPO, "fuzz", "corpus", harness)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data.encode("latin-1"))


REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
    parser_like = ["fuzz_structural_index", "fuzz_runtime_parser",
                   "fuzz_serializer_roundtrip"]

    for ver, bs in BEGIN.items():
        for h in parser_like:
            write(h, f"nos_{ver}", nos(bs, 1))
            write(h, f"exec_{ver}", exec_report(bs, 2))
            write(h, f"logon_{ver}", logon(bs, 1))
        # session harness: session-layer messages
        write("fuzz_session_input", f"logon_{ver}", logon(bs, 2))
        write("fuzz_session_input", f"heartbeat_{ver}", heartbeat(bs, 3))
        write("fuzz_session_input", f"resend_{ver}", resend_request(bs, 2))
        write("fuzz_session_input", f"exec_{ver}", exec_report(bs, 2))

    for name, data in MALFORMED.items():
        for h in HARNESSES:
            write(h, f"bad_{name}", data)

    # Placeholder so regressions/ is tracked; real crash inputs land here.
    for h in HARNESSES:
        rd = os.path.join(REPO, "fuzz", "corpus", h, "regressions")
        os.makedirs(rd, exist_ok=True)
        keep = os.path.join(rd, ".gitkeep")
        if not os.path.exists(keep):
            open(keep, "w").close()

    print("Seed corpus written under fuzz/corpus/<harness>/")


if __name__ == "__main__":
    main()
