// fuzz_session_input.cpp
//
// libFuzzer harness: framed bytes -> SessionManager state machine.
//
// This is the deepest target: the session layer runs the admin protocol
// (Logon/Logout/Heartbeat/ResendRequest/SequenceReset), sequence-number
// tracking, and CompID validation over whatever the counterparty sends. It is
// declared noexcept end to end, so any input that throws or aborts is a bug.
//
// The harness reaches an established session, then feeds the fuzz input as if
// it arrived on the wire. Callbacks are wired to no-ops that just record, so
// the app-message and send paths get walked without external side effects.
//
// Build: clang++ -std=c++23 -fsanitize=fuzzer,address,undefined ...

#include "nexusfix/session/session_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

using nfx::SessionCallbacks;
using nfx::SessionConfig;
using nfx::SessionError;
using nfx::SessionManager;
using nfx::SessionState;

namespace {

// Build a minimal valid FIX 4.4 Logon so the fuzzer starts from an Active
// session rather than re-discovering the handshake every run. Body length and
// checksum are computed so on_data_received accepts it.
std::string build_logon(uint32_t seq) {
    std::string body;
    body += "35=A\x01";
    body += "49=TARGET\x01";  // counterparty is our target
    body += "56=SENDER\x01";
    body += "34=" + std::to_string(seq) + "\x01";
    body += "52=20231215-10:30:00\x01";
    body += "98=0\x01";
    body += "108=30\x01";

    std::string head = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01";
    std::string frame = head + body;

    unsigned sum = 0;
    for (unsigned char c : frame) sum += c;
    char cs[8];
    std::snprintf(cs, sizeof(cs), "10=%03u\x01", sum % 256);
    frame += cs;
    return frame;
}

// SessionManager is non-copyable and non-movable (it owns session state), so
// configure it in place rather than returning it.
void drive_to_active(SessionManager& session) {
    SessionCallbacks cbs;
    cbs.on_send = [](std::span<const char>) -> bool { return true; };
    cbs.on_state_change = [](SessionState, SessionState) {};
    cbs.on_app_message = [](const nfx::ParsedMessage&) {};
    cbs.on_logon = []() {};
    cbs.on_logout = [](std::string_view) {};
    cbs.on_error = [](const SessionError&) {};
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    const std::string logon = build_logon(1);
    session.on_data_received(std::span<const char>{logon.data(), logon.size()});
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    SessionConfig cfg{};
    cfg.sender_comp_id = "SENDER";
    cfg.target_comp_id = "TARGET";
    cfg.heart_bt_int = 30;
    cfg.check_latency = false;

    SessionManager session(cfg);
    drive_to_active(session);

    std::span<const char> bytes{reinterpret_cast<const char*>(data), size};
    session.on_data_received(bytes);

    // Feeding twice exercises the resynchronization / stateful branches that
    // only trigger when a second chunk arrives after the first mutated state.
    session.on_data_received(bytes);

    (void)session.state();
    return 0;
}
