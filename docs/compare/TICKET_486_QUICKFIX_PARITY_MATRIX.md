# TICKET_486: QuickFIX Session-Level Parity Matrix

**Date**: 2026-06-19
**Test File**: `tests/test_quickfix_parity.cpp`
**Test Count**: 39 test cases, 157 assertions
**Regressions**: 0 (full suite green)

---

## Approach

Protocol knowledge extracted from two QuickFIX sources:

1. **Acceptance test definitions** (`spec/definition/server/*.def`): scenario scripts encoding 20+ years of production edge cases, translated into native Catch2 test cases.
2. **Session.cpp defensive logic** (~3000 lines): defensive branches forced by production incidents, implemented as SessionManager improvements + tests.

No QuickFIX runtime dependency. All tests are pure NexusFIX Catch2 tests.

---

## Parity Matrix

### Category 1: Logon Scenarios

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-1A | Normal logon (initiator) | `1a_ValidLogon.def` | PASS |
| QFP-1B | Normal logon (acceptor) | `1a_ValidLogon.def` | PASS |
| QFP-1C | Wrong SenderCompID -> reject + disconnect | `isCorrectCompID()` | PASS |
| QFP-1D | Wrong TargetCompID -> reject + disconnect | `isCorrectCompID()` | PASS |
| QFP-1E | CompID validation disabled | Config option | PASS |
| QFP-1F | ResetSeqNumFlag=Y | `14e_ResetSeqNum.def` | PASS |

### Category 2: Sequence Number Handling

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-2A | Normal sequential messages | `2a_MsgSeqNumCorrect.def` | PASS |
| QFP-2B | Gap -> ResendRequest | `2b_MsgSeqNumTooHigh.def` | PASS |
| QFP-2C | Seq too low, no PossDup -> error | `2c_MsgSeqNumTooLow.def` | PASS |
| QFP-2D | Seq too low, PossDup=Y -> accept | `2d_PossDupMsgSeqNum.def` | PASS |
| QFP-2E | SequenceReset-Reset (hard) | `2e_SequenceReset.def` | PASS |
| QFP-2F | SequenceReset-GapFill | `2f_GapFillMsgSeqNum.def` | PASS |
| QFP-2G | GapFill NewSeqNo < expected -> reject | `nextSequenceReset()` | PASS |
| QFP-2H | ResendRequest -> replay from store | `2g_ResendRequest.def` | PASS |
| QFP-2I | ResendRequest -> GapFill (no store) | `generateSequenceReset()` | PASS |

### Category 3: Heartbeat / TestRequest

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-3A | Heartbeat after HeartBtInt inactivity | `3_HeartBtInt.def` | PASS |
| QFP-3B | TestRequest after 1.5x HeartBtInt | `3_HeartBtInt.def` | PASS |
| QFP-3C | Timeout after 2x HeartBtInt | `3_HeartBtInt.def` | PASS |
| QFP-3D | TestRequest response carries TestReqID | `2k_TestRequest.def` | PASS |
| QFP-3E | Receiving heartbeat resets timer | Timer logic | PASS |

### Category 4: Reject (35=3)

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-4A | Reject for CompID mismatch | `isCorrectCompID()` | PASS |
| QFP-4B | Received Reject triggers error callback | `nextReject()` | PASS |
| QFP-4C | Reject references correct RefSeqNum | `generateReject()` | PASS |

### Category 5: Logout

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-5A | Initiator-side graceful logout | `5_Logout.def` | PASS |
| QFP-5B | Acceptor-side logout response | `5_Logout.def` | PASS |
| QFP-5C | Logout during LogoutPending -> Disconnected | State machine | PASS |
| QFP-5D | Cannot initiate logout when not Active | State machine | PASS |

### Category 6: Message Integrity

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-6A | Invalid checksum -> parse error | `validate()` | PASS |
| QFP-6B | Invalid BodyLength -> parse error | `validate()` | PASS |
| QFP-6C | Garbled message -> error callback | `validate()` | PASS |

### Category 7: PossDupFlag / Resend

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-7A | PossDup=Y with OrigSendingTime -> accept | `doPossDup()` | PASS |
| QFP-7B | PossDup=Y without OrigSendingTime -> reject | `doPossDup()` | PASS |

### Category 8: SendingTime Accuracy

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-8A | SendingTime within max_latency -> accept | `isGoodTime()` | PASS |
| QFP-8B | SendingTime too old -> reject + logout | `isGoodTime()` | PASS |
| QFP-8C | check_latency=false disables check | Config option | PASS |

### Category 9: Application Messages

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-9A | App message dispatched via callback | `fromApp()` | PASS |
| QFP-9B | Cannot send app msg when not Active | `sentMessages()` | PASS |

### Category 10: Statistics

| ID | Behavior | QuickFIX Source | NexusFIX Status |
|----|----------|-----------------|-----------------|
| QFP-10A | Stats track sent/received correctly | N/A | PASS |
| QFP-10B | Reject stats tracked | N/A | PASS |

---

## Intentional Omissions

| QuickFIX Behavior | Rationale |
|----|----------|
| BusinessMessageReject (35=j) | Not in Phase 1 scope. NexusFIX focuses on session-level protocol. |
| SessionTime / LogonTime windows | Not implemented. Production deployments use external scheduling. |
| DataDictionary validation | NexusFIX is schema-agnostic by design. |
| SSL/TLS handshake scenarios | Transport-level, orthogonal to session protocol. |
| Multiple session management | Single-session focus in Phase 1. |

---

## Implementation Changes

### SessionManager Enhancements

| Enhancement | QuickFIX Source Method | Implementation |
|-------------|----------------------|----------------|
| CompID validation | `isCorrectCompID()` | `on_data_received()` pre-check |
| SendingTime accuracy | `isGoodTime()` | `is_sending_time_accurate()` |
| SequenceReset validation | `nextSequenceReset()` | `handle_sequence_reset()` reject |
| `send_reject()` helper | `generateReject()` | Builds Reject(35=3) with reason |
| PossDup+OrigSendingTime | `doPossDup()` | `on_data_received()` pre-check |

### New SessionErrorCodes

| Code | Value | Purpose |
|------|-------|---------|
| CompIdMismatch | 9 | SenderCompID/TargetCompID mismatch |
| SendingTimeAccuracy | 10 | SendingTime outside max_latency |

### Bug Fix: HeartbeatTimer integer division truncation

The 1.5x HeartBtInt multiplier for TestRequest threshold used integer division (`interval_.count() / 2`), which truncated to 0 for HeartBtInt=1. Fixed by computing in milliseconds: `interval_ms * 3 / 2`.

### Parser Enhancement

`parse_header()` loop limit increased from 7 (required fields only) to 10 (`MAX_HEADER_FIELDS`) to parse optional header fields: PossDupFlag (43), PossResend (97), OrigSendingTime (122).

---

## Coverage Summary

| Metric | Value |
|--------|-------|
| Test categories | 10 |
| Test cases | 39 |
| Assertions | 157 |
| QuickFIX .def files covered | ~15 |
| Session.cpp defensive branches covered | 5 |
| SessionManager methods added | 3 (send_reject, send_logout_and_disconnect, is_sending_time_accurate) |
| Bugs found and fixed | 1 (HeartbeatTimer integer division) |
| Regressions | 0 |
