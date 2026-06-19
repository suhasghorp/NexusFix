// test_quickfix_parity.cpp
// TICKET_486: QuickFIX Session-Level Acceptance Test Parity
//
// Tests derived from QuickFIX acceptance test definitions and Session.cpp
// defensive logic. Each test encodes a FIX spec behavior validated by
// 20+ years of QuickFIX production use.

#include <catch2/catch_test_macros.hpp>
#include <span>
#include <string>
#include <vector>

#include "nexusfix/nexusfix.hpp"

using namespace nfx;

// ============================================================================
// Test Infrastructure
// ============================================================================

namespace {

class SpyStore final : public store::IMessageStore {
public:
    struct Entry {
        uint32_t seq;
        std::vector<char> data;
    };
    std::vector<Entry> entries;

    [[nodiscard]] bool store(uint32_t seq,
                             std::span<const char> msg) noexcept override {
        entries.push_back({seq, {msg.begin(), msg.end()}});
        return true;
    }
    [[nodiscard]] std::optional<std::vector<char>>
        retrieve(uint32_t) const noexcept override { return std::nullopt; }
    [[nodiscard]] std::vector<std::vector<char>>
        retrieve_range(uint32_t, uint32_t) const noexcept override { return {}; }
    void set_next_sender_seq_num(uint32_t) noexcept override {}
    void set_next_target_seq_num(uint32_t) noexcept override {}
    [[nodiscard]] uint32_t get_next_sender_seq_num() const noexcept override { return 1; }
    [[nodiscard]] uint32_t get_next_target_seq_num() const noexcept override { return 1; }
    void reset() noexcept override { entries.clear(); }
    void flush() noexcept override {}
    [[nodiscard]] std::string_view session_id() const noexcept override { return "TEST"; }
    [[nodiscard]] Stats stats() const noexcept override { return {}; }
};

class ReplayStore final : public store::IMessageStore {
public:
    std::vector<std::vector<char>> stored;

    [[nodiscard]] bool store(uint32_t,
                             std::span<const char> msg) noexcept override {
        stored.push_back({msg.begin(), msg.end()});
        return true;
    }
    [[nodiscard]] std::optional<std::vector<char>>
        retrieve(uint32_t seq) const noexcept override {
        if (seq > 0 && seq <= stored.size())
            return stored[seq - 1];
        return std::nullopt;
    }
    [[nodiscard]] std::vector<std::vector<char>>
        retrieve_range(uint32_t begin, uint32_t end) const noexcept override {
        std::vector<std::vector<char>> result;
        uint32_t last = (end == 0) ? static_cast<uint32_t>(stored.size()) : end;
        for (uint32_t i = begin; i <= last && i <= stored.size(); ++i) {
            result.push_back(stored[i - 1]);
        }
        return result;
    }
    void set_next_sender_seq_num(uint32_t) noexcept override {}
    void set_next_target_seq_num(uint32_t) noexcept override {}
    [[nodiscard]] uint32_t get_next_sender_seq_num() const noexcept override { return 1; }
    [[nodiscard]] uint32_t get_next_target_seq_num() const noexcept override { return 1; }
    void reset() noexcept override { stored.clear(); }
    void flush() noexcept override {}
    [[nodiscard]] std::string_view session_id() const noexcept override { return "REPLAY"; }
    [[nodiscard]] Stats stats() const noexcept override { return {}; }
};

struct SentCapture {
    std::vector<std::string> messages;

    bool on_send(std::span<const char> data) {
        messages.emplace_back(data.data(), data.size());
        return true;
    }

    void clear() { messages.clear(); }

    [[nodiscard]] bool has_msg_type(char type) const {
        for (const auto& m : messages) {
            auto parsed = IndexedParser::parse(
                std::span<const char>{m.data(), m.size()});
            if (parsed.has_value() && parsed->msg_type() == type)
                return true;
        }
        return false;
    }

    [[nodiscard]] std::string find_msg_type(char type) const {
        for (const auto& m : messages) {
            auto parsed = IndexedParser::parse(
                std::span<const char>{m.data(), m.size()});
            if (parsed.has_value() && parsed->msg_type() == type)
                return m;
        }
        return {};
    }

    [[nodiscard]] size_t count_msg_type(char type) const {
        size_t count = 0;
        for (const auto& m : messages) {
            auto parsed = IndexedParser::parse(
                std::span<const char>{m.data(), m.size()});
            if (parsed.has_value() && parsed->msg_type() == type)
                ++count;
        }
        return count;
    }
};

std::string build_logon(std::string_view sender, std::string_view target,
                        uint32_t seq_num, int heart_bt_int = 30,
                        bool reset_flag = false) {
    MessageAssembler asm_;
    auto msg = fix44::Logon::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .encrypt_method(0)
        .heart_bt_int(heart_bt_int)
        .reset_seq_num_flag(reset_flag)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_logout(std::string_view sender, std::string_view target,
                         uint32_t seq_num, std::string_view text = "") {
    MessageAssembler asm_;
    auto msg = fix44::Logout::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .text(text)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_heartbeat(std::string_view sender, std::string_view target,
                            uint32_t seq_num, std::string_view test_req_id = "") {
    MessageAssembler asm_;
    auto msg = fix44::Heartbeat::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .test_req_id(test_req_id)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_test_request(std::string_view sender, std::string_view target,
                               uint32_t seq_num, std::string_view test_req_id) {
    MessageAssembler asm_;
    auto msg = fix44::TestRequest::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .test_req_id(test_req_id)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_sequence_reset(std::string_view sender, std::string_view target,
                                 uint32_t seq_num, uint32_t new_seq_no,
                                 bool gap_fill = false) {
    MessageAssembler asm_;
    auto msg = fix44::SequenceReset::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .new_seq_no(new_seq_no)
        .gap_fill_flag(gap_fill)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_resend_request(std::string_view sender, std::string_view target,
                                 uint32_t seq_num,
                                 uint32_t begin_seq, uint32_t end_seq) {
    MessageAssembler asm_;
    auto msg = fix44::ResendRequest::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .begin_seq_no(begin_seq)
        .end_seq_no(end_seq)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_reject(std::string_view sender, std::string_view target,
                         uint32_t seq_num, uint32_t ref_seq,
                         int reason = 0, std::string_view text = "") {
    MessageAssembler asm_;
    auto msg = fix44::Reject::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq_num)
        .sending_time("20260619-12:00:00.000")
        .ref_seq_num(ref_seq)
        .session_reject_reason(reason)
        .text(text)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_nos(std::string_view sender, std::string_view target,
                      uint32_t seq_num) {
    MessageAssembler asm_;
    asm_.start()
        .field(tag::MsgType::value, msg_type::NewOrderSingle)
        .field(tag::SenderCompID::value, sender)
        .field(tag::TargetCompID::value, target)
        .field(tag::MsgSeqNum::value, static_cast<int64_t>(seq_num))
        .field(tag::SendingTime::value, "20260619-12:00:00.000")
        .field(11, "ORD001")   // ClOrdID
        .field(21, '1')        // HandlInst
        .field(55, "AAPL")    // Symbol
        .field(54, '1')        // Side=Buy
        .field(60, "20260619-12:00:00.000")  // TransactTime
        .field(38, int64_t{100})   // OrderQty
        .field(40, '2')        // OrdType=Limit
        .field(44, "150.00");  // Price
    auto msg = asm_.finish();
    return std::string(msg.data(), msg.size());
}

std::string build_poss_dup_msg(std::string_view sender, std::string_view target,
                               uint32_t seq_num, bool with_orig_time = true) {
    MessageAssembler asm_;
    asm_.start()
        .field(tag::MsgType::value, msg_type::Heartbeat)
        .field(tag::SenderCompID::value, sender)
        .field(tag::TargetCompID::value, target)
        .field(tag::MsgSeqNum::value, static_cast<int64_t>(seq_num))
        .field(tag::SendingTime::value, "20260619-12:00:00.000")
        .field(tag::PossDupFlag::value, 'Y');
    if (with_orig_time) {
        asm_.field(tag::OrigSendingTime::value, "20260619-11:59:00.000");
    }
    auto msg = asm_.finish();
    return std::string(msg.data(), msg.size());
}

struct TestSession {
    SentCapture capture;
    std::vector<SessionError> errors;
    std::vector<std::pair<SessionState, SessionState>> transitions;
    bool logon_called{false};
    std::string logout_text;

    static SessionConfig make_config(std::string_view sender,
                                     std::string_view target,
                                     int hb_int) {
        SessionConfig c{};
        c.sender_comp_id = sender;
        c.target_comp_id = target;
        c.heart_bt_int = hb_int;
        c.check_latency = false;
        return c;
    }

    SessionManager session;

    TestSession(std::string_view sender = "SENDER",
                std::string_view target = "TARGET",
                int hb_int = 30)
        : session(make_config(sender, target, hb_int))
    {
        SessionCallbacks cbs;
        cbs.on_send = [this](std::span<const char> d) -> bool {
            return capture.on_send(d);
        };
        cbs.on_error = [this](const SessionError& e) {
            errors.push_back(e);
        };
        cbs.on_state_change = [this](SessionState from, SessionState to) {
            transitions.emplace_back(from, to);
        };
        cbs.on_logon = [this]() { logon_called = true; };
        cbs.on_logout = [this](std::string_view text) {
            logout_text = std::string(text);
        };
        session.set_callbacks(std::move(cbs));
    }

    void establish() {
        session.on_connect();
        auto result = session.initiate_logon();
        REQUIRE(result.has_value());
        capture.clear();

        auto resp = build_logon("TARGET", "SENDER", 1);
        session.on_data_received(
            std::span<const char>{resp.data(), resp.size()});
        REQUIRE(session.state() == SessionState::Active);
        capture.clear();
        errors.clear();
        transitions.clear();
    }

    void feed(const std::string& msg) {
        session.on_data_received(
            std::span<const char>{msg.data(), msg.size()});
    }
};

} // anonymous namespace

// ============================================================================
// Category 1: Logon Scenarios
// ============================================================================

TEST_CASE("QFP-1A: Normal logon handshake (initiator)", "[qfp][logon]") {
    TestSession ts;
    ts.session.on_connect();
    REQUIRE(ts.session.state() == SessionState::SocketConnected);

    auto result = ts.session.initiate_logon();
    REQUIRE(result.has_value());
    REQUIRE(ts.session.state() == SessionState::LogonSent);
    REQUIRE(ts.capture.has_msg_type(msg_type::Logon));

    auto logon_resp = build_logon("TARGET", "SENDER", 1);
    ts.feed(logon_resp);
    REQUIRE(ts.session.state() == SessionState::Active);
    REQUIRE(ts.logon_called);
}

TEST_CASE("QFP-1B: Normal logon handshake (acceptor)", "[qfp][logon]") {
    TestSession ts;
    ts.session.on_connect();

    auto logon_req = build_logon("TARGET", "SENDER", 1);
    ts.feed(logon_req);

    REQUIRE(ts.session.state() == SessionState::Active);
    REQUIRE(ts.capture.has_msg_type(msg_type::Logon));
    REQUIRE(ts.logon_called);
}

TEST_CASE("QFP-1C: Logon with wrong SenderCompID", "[qfp][logon][compid]") {
    TestSession ts;
    ts.establish();

    auto msg = build_heartbeat("WRONG_SENDER", "SENDER", 2);
    ts.feed(msg);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));
    REQUIRE(ts.capture.has_msg_type(msg_type::Logout));

    auto reject_msg = ts.capture.find_msg_type(msg_type::Reject);
    auto parsed = IndexedParser::parse(
        std::span<const char>{reject_msg.data(), reject_msg.size()});
    REQUIRE(parsed.has_value());
    auto reason = parsed->get_int(373);
    REQUIRE(reason.has_value());
    REQUIRE(*reason == 9);
}

TEST_CASE("QFP-1D: Logon with wrong TargetCompID", "[qfp][logon][compid]") {
    TestSession ts;
    ts.establish();

    auto msg = build_heartbeat("TARGET", "WRONG_TARGET", 2);
    ts.feed(msg);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));
    REQUIRE(ts.capture.has_msg_type(msg_type::Logout));
}

TEST_CASE("QFP-1E: CompID validation disabled", "[qfp][logon][compid]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.validate_comp_ids = false;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);
    capture.clear();

    auto msg = build_heartbeat("WRONG", "SENDER", 2);
    session.on_data_received(
        std::span<const char>{msg.data(), msg.size()});

    REQUIRE_FALSE(capture.has_msg_type(msg_type::Reject));
}

TEST_CASE("QFP-1F: Logon with ResetSeqNumFlag=Y", "[qfp][logon][reset]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.reset_seq_num_on_logon = true;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    auto result = session.initiate_logon();
    REQUIRE(result.has_value());

    REQUIRE(session.sequences().current_outbound() == 2);

    auto logon_resp = build_logon("TARGET", "SENDER", 1, 30, true);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);
}

// ============================================================================
// Category 2: Sequence Number Handling
// ============================================================================

TEST_CASE("QFP-2A: Normal sequential messages accepted", "[qfp][sequence]") {
    TestSession ts;
    ts.establish();

    auto hb2 = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb2);
    REQUIRE(ts.errors.empty());
    REQUIRE(ts.session.sequences().expected_inbound() == 3);

    auto hb3 = build_heartbeat("TARGET", "SENDER", 3);
    ts.feed(hb3);
    REQUIRE(ts.errors.empty());
    REQUIRE(ts.session.sequences().expected_inbound() == 4);
}

TEST_CASE("QFP-2B: Gap detected -> ResendRequest sent", "[qfp][sequence][gap]") {
    TestSession ts;
    ts.establish();

    auto hb5 = build_heartbeat("TARGET", "SENDER", 5);
    ts.feed(hb5);

    REQUIRE(ts.capture.has_msg_type(msg_type::ResendRequest));

    auto rr = ts.capture.find_msg_type(msg_type::ResendRequest);
    auto parsed = IndexedParser::parse(
        std::span<const char>{rr.data(), rr.size()});
    REQUIRE(parsed.has_value());
    auto begin_seq = parsed->get_int(7);
    REQUIRE(begin_seq.has_value());
    REQUIRE(*begin_seq == 2);
}

TEST_CASE("QFP-2C: Sequence too low without PossDupFlag -> error", "[qfp][sequence]") {
    TestSession ts;
    ts.establish();

    auto hb2 = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb2);
    REQUIRE(ts.errors.empty());

    auto hb1_dup = build_heartbeat("TARGET", "SENDER", 1);
    ts.feed(hb1_dup);
    REQUIRE_FALSE(ts.errors.empty());
    REQUIRE(ts.errors.back().code == SessionErrorCode::SequenceGap);
}

TEST_CASE("QFP-2D: Sequence too low with PossDupFlag=Y -> accepted", "[qfp][sequence]") {
    TestSession ts;
    ts.establish();

    auto hb2 = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb2);

    auto dup = build_poss_dup_msg("TARGET", "SENDER", 1, true);
    ts.feed(dup);
    REQUIRE(ts.errors.empty());
}

TEST_CASE("QFP-2E: SequenceReset-Reset (hard reset)", "[qfp][sequence][reset]") {
    TestSession ts;
    ts.establish();

    auto sr = build_sequence_reset("TARGET", "SENDER", 2, 10, false);
    ts.feed(sr);

    REQUIRE(ts.session.sequences().expected_inbound() == 10);
    REQUIRE(ts.errors.empty());
}

TEST_CASE("QFP-2F: SequenceReset-GapFill", "[qfp][sequence][gapfill]") {
    TestSession ts;
    ts.establish();

    auto sr = build_sequence_reset("TARGET", "SENDER", 2, 5, true);
    ts.feed(sr);

    REQUIRE(ts.session.sequences().expected_inbound() == 5);
    REQUIRE(ts.errors.empty());
}

TEST_CASE("QFP-2G: SequenceReset-GapFill with NewSeqNo too low -> reject",
          "[qfp][sequence][gapfill]") {
    TestSession ts;
    ts.establish();

    auto hb2 = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb2);
    REQUIRE(ts.session.sequences().expected_inbound() == 3);

    auto sr = build_sequence_reset("TARGET", "SENDER", 3, 2, true);
    ts.feed(sr);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));

    auto reject_str = ts.capture.find_msg_type(msg_type::Reject);
    auto parsed = IndexedParser::parse(
        std::span<const char>{reject_str.data(), reject_str.size()});
    REQUIRE(parsed.has_value());
    auto reason = parsed->get_int(373);
    REQUIRE(reason.has_value());
    REQUIRE(*reason == 5);
}

TEST_CASE("QFP-2H: ResendRequest -> replay from store", "[qfp][sequence][resend]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.check_latency = false;
    SessionManager session(config);

    ReplayStore replay_store;
    session.set_message_store(&replay_store);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);

    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ORD001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260619-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.0));
    (void)session.send_app_message(nos_builder);

    capture.clear();

    auto rr = build_resend_request("TARGET", "SENDER", 2, 2, 2);
    session.on_data_received(
        std::span<const char>{rr.data(), rr.size()});

    bool found_replay = false;
    for (const auto& m : capture.messages) {
        auto parsed = IndexedParser::parse(
            std::span<const char>{m.data(), m.size()});
        if (parsed.has_value() && parsed->header().poss_dup_flag) {
            found_replay = true;
            REQUIRE_FALSE(parsed->header().orig_sending_time.empty());
        }
    }
    REQUIRE(found_replay);
}

TEST_CASE("QFP-2I: ResendRequest -> GapFill when no stored messages",
          "[qfp][sequence][resend]") {
    TestSession ts;
    SpyStore spy;
    ts.session.set_message_store(&spy);
    ts.establish();

    auto rr = build_resend_request("TARGET", "SENDER", 2, 1, 0);
    ts.feed(rr);

    REQUIRE(ts.capture.has_msg_type(msg_type::SequenceReset));

    auto sr = ts.capture.find_msg_type(msg_type::SequenceReset);
    auto parsed = IndexedParser::parse(
        std::span<const char>{sr.data(), sr.size()});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->get_char(123) == 'Y');
}

// ============================================================================
// Category 3: Heartbeat / TestRequest
// ============================================================================

TEST_CASE("QFP-3A: Heartbeat sent after HeartBtInt inactivity",
          "[qfp][heartbeat]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.heart_bt_int = 1;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1, 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);
    capture.clear();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    session.on_timer_tick();

    REQUIRE(capture.has_msg_type(msg_type::Heartbeat));
}

TEST_CASE("QFP-3B: TestRequest sent after 1.5x HeartBtInt no receive",
          "[qfp][heartbeat]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.heart_bt_int = 1;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1, 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);
    capture.clear();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    session.on_timer_tick();
    capture.clear();

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    session.on_timer_tick();

    REQUIRE(capture.has_msg_type(msg_type::TestRequest));
}

TEST_CASE("QFP-3C: Heartbeat timeout after 2x HeartBtInt",
          "[qfp][heartbeat]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.heart_bt_int = 1;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1, 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);

    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    session.on_timer_tick();

    REQUIRE(session.state() == SessionState::Error);
}

TEST_CASE("QFP-3D: TestRequest response carries TestReqID",
          "[qfp][heartbeat]") {
    TestSession ts;
    ts.establish();

    auto tr = build_test_request("TARGET", "SENDER", 2, "MY_TEST_123");
    ts.feed(tr);

    REQUIRE(ts.capture.has_msg_type(msg_type::Heartbeat));

    auto hb = ts.capture.find_msg_type(msg_type::Heartbeat);
    auto parsed = IndexedParser::parse(
        std::span<const char>{hb.data(), hb.size()});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->get_string(tag::TestReqID::value) == "MY_TEST_123");
}

TEST_CASE("QFP-3E: Receiving heartbeat resets timer",
          "[qfp][heartbeat]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.heart_bt_int = 1;
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1, 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    auto hb = build_heartbeat("TARGET", "SENDER", 2);
    session.on_data_received(
        std::span<const char>{hb.data(), hb.size()});

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    session.on_timer_tick();

    REQUIRE(session.state() == SessionState::Active);
}

// ============================================================================
// Category 4: Reject (35=3)
// ============================================================================

TEST_CASE("QFP-4A: Session sends Reject for CompID mismatch",
          "[qfp][reject][compid]") {
    TestSession ts;
    ts.establish();

    auto bad_msg = build_heartbeat("WRONG_SENDER", "SENDER", 2);
    ts.feed(bad_msg);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));
    REQUIRE(ts.session.stats().rejects_sent == 1);

    auto reject_str = ts.capture.find_msg_type(msg_type::Reject);
    auto parsed = IndexedParser::parse(
        std::span<const char>{reject_str.data(), reject_str.size()});
    REQUIRE(parsed.has_value());

    auto ref_seq = parsed->get_int(tag::RefSeqNum::value);
    REQUIRE(ref_seq.has_value());
    REQUIRE(*ref_seq == 2);

    auto reason = parsed->get_int(373);
    REQUIRE(reason.has_value());
    REQUIRE(*reason == 9);
}

TEST_CASE("QFP-4B: Received Reject triggers error callback",
          "[qfp][reject]") {
    TestSession ts;
    ts.establish();

    auto rej = build_reject("TARGET", "SENDER", 2, 1, 0, "Test reject");
    ts.feed(rej);

    REQUIRE_FALSE(ts.errors.empty());
}

TEST_CASE("QFP-4C: Reject references correct RefSeqNum",
          "[qfp][reject]") {
    TestSession ts;
    ts.establish();

    auto hb = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb);

    auto bad = build_heartbeat("WRONG", "SENDER", 3);
    ts.feed(bad);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));

    auto reject_str = ts.capture.find_msg_type(msg_type::Reject);
    auto parsed = IndexedParser::parse(
        std::span<const char>{reject_str.data(), reject_str.size()});
    REQUIRE(parsed.has_value());

    auto ref_seq = parsed->get_int(tag::RefSeqNum::value);
    REQUIRE(ref_seq.has_value());
    REQUIRE(*ref_seq == 3);
}

// ============================================================================
// Category 5: Logout
// ============================================================================

TEST_CASE("QFP-5A: Initiator-side graceful logout", "[qfp][logout]") {
    TestSession ts;
    ts.establish();

    auto result = ts.session.initiate_logout("Done trading");
    REQUIRE(result.has_value());
    REQUIRE(ts.session.state() == SessionState::LogoutPending);
    REQUIRE(ts.capture.has_msg_type(msg_type::Logout));

    auto logout_resp = build_logout("TARGET", "SENDER", 2);
    ts.feed(logout_resp);

    REQUIRE(ts.session.state() == SessionState::Disconnected);
}

TEST_CASE("QFP-5B: Acceptor-side logout response", "[qfp][logout]") {
    TestSession ts;
    ts.establish();

    auto logout_req = build_logout("TARGET", "SENDER", 2, "Bye");
    ts.feed(logout_req);

    REQUIRE(ts.capture.has_msg_type(msg_type::Logout));
    REQUIRE(ts.logout_text == "Bye");
}

TEST_CASE("QFP-5C: Logout during LogoutPending -> Disconnected",
          "[qfp][logout]") {
    TestSession ts;
    ts.establish();

    (void)ts.session.initiate_logout();
    REQUIRE(ts.session.state() == SessionState::LogoutPending);

    auto logout_resp = build_logout("TARGET", "SENDER", 2);
    ts.feed(logout_resp);

    REQUIRE(ts.session.state() == SessionState::Disconnected);
}

TEST_CASE("QFP-5D: Cannot initiate logout when not Active",
          "[qfp][logout]") {
    TestSession ts;

    auto result = ts.session.initiate_logout();
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == SessionErrorCode::InvalidState);
}

// ============================================================================
// Category 6: Message Integrity
// ============================================================================

TEST_CASE("QFP-6A: Invalid checksum -> parse error callback",
          "[qfp][integrity]") {
    TestSession ts;
    ts.establish();

    std::string bad_msg =
        "8=FIX.4.4\x01"
        "9=49\x01"
        "35=0\x01"
        "49=TARGET\x01"
        "56=SENDER\x01"
        "34=2\x01"
        "52=20260619-12:00:00\x01"
        "10=999\x01";

    ts.feed(bad_msg);

    REQUIRE_FALSE(ts.errors.empty());
}

TEST_CASE("QFP-6B: Invalid BodyLength -> parse error callback",
          "[qfp][integrity]") {
    TestSession ts;
    ts.establish();

    std::string bad_msg =
        "8=FIX.4.4\x01"
        "9=999\x01"
        "35=0\x01"
        "49=TARGET\x01"
        "56=SENDER\x01"
        "34=2\x01"
        "52=20260619-12:00:00\x01"
        "10=000\x01";

    ts.feed(bad_msg);

    REQUIRE_FALSE(ts.errors.empty());
}

TEST_CASE("QFP-6C: Garbled message -> error callback",
          "[qfp][integrity]") {
    TestSession ts;
    ts.establish();

    std::string garbled = "THIS IS NOT A FIX MESSAGE";

    ts.feed(garbled);

    REQUIRE_FALSE(ts.errors.empty());
}

// ============================================================================
// Category 7: PossDupFlag / Resend
// ============================================================================

TEST_CASE("QFP-7A: PossDupFlag=Y with OrigSendingTime accepted for TooLow seq",
          "[qfp][possdup]") {
    TestSession ts;
    ts.establish();

    auto hb2 = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb2);
    REQUIRE(ts.session.sequences().expected_inbound() == 3);

    auto dup = build_poss_dup_msg("TARGET", "SENDER", 1, true);
    ts.feed(dup);
    REQUIRE(ts.errors.empty());
}

TEST_CASE("QFP-7B: PossDupFlag=Y without OrigSendingTime -> reject",
          "[qfp][possdup]") {
    TestSession ts;
    ts.establish();

    auto bad_dup = build_poss_dup_msg("TARGET", "SENDER", 2, false);
    ts.feed(bad_dup);

    REQUIRE(ts.capture.has_msg_type(msg_type::Reject));

    auto reject_str = ts.capture.find_msg_type(msg_type::Reject);
    auto parsed = IndexedParser::parse(
        std::span<const char>{reject_str.data(), reject_str.size()});
    REQUIRE(parsed.has_value());

    auto reason = parsed->get_int(373);
    REQUIRE(reason.has_value());
    REQUIRE(*reason == 1);

    auto ref_tag = parsed->get_int(371);
    REQUIRE(ref_tag.has_value());
    REQUIRE(*ref_tag == 122);
}

// ============================================================================
// Category 8: SendingTime Accuracy (QuickFIX isGoodTime)
// ============================================================================

TEST_CASE("QFP-8A: SendingTime within max_latency accepted",
          "[qfp][sendingtime]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.check_latency = true;
    config.max_latency = 120;
    SessionManager session(config);

    SentCapture capture;
    std::vector<SessionError> errors;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    cbs.on_error = [&](const SessionError& e) { errors.push_back(e); };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();

    // Build logon response with current time
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d:%02d:%02d.000",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string_view current_ts{ts_buf};

    MessageAssembler asm_;
    auto logon_msg = fix44::Logon::Builder{}
        .sender_comp_id("TARGET")
        .target_comp_id("SENDER")
        .msg_seq_num(1)
        .sending_time(current_ts)
        .encrypt_method(0)
        .heart_bt_int(30)
        .build(asm_);
    std::string logon_str(logon_msg.data(), logon_msg.size());

    session.on_data_received(
        std::span<const char>{logon_str.data(), logon_str.size()});
    REQUIRE(session.state() == SessionState::Active);
    capture.clear();

    // Now send a heartbeat with current time
    MessageAssembler asm2;
    auto hb_msg = fix44::Heartbeat::Builder{}
        .sender_comp_id("TARGET")
        .target_comp_id("SENDER")
        .msg_seq_num(2)
        .sending_time(current_ts)
        .build(asm2);
    std::string hb_str(hb_msg.data(), hb_msg.size());

    session.on_data_received(
        std::span<const char>{hb_str.data(), hb_str.size()});

    REQUIRE_FALSE(capture.has_msg_type(msg_type::Reject));
}

TEST_CASE("QFP-8B: SendingTime too old -> reject + logout",
          "[qfp][sendingtime]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.check_latency = true;
    config.max_latency = 120;
    SessionManager session(config);

    SentCapture capture;
    std::vector<SessionError> errors;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    cbs.on_error = [&](const SessionError& e) { errors.push_back(e); };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();

    // Build logon with current time (to get past the logon phase)
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d:%02d:%02d.000",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    MessageAssembler asm_;
    auto logon_msg = fix44::Logon::Builder{}
        .sender_comp_id("TARGET")
        .target_comp_id("SENDER")
        .msg_seq_num(1)
        .sending_time(std::string_view{ts_buf})
        .encrypt_method(0)
        .heart_bt_int(30)
        .build(asm_);
    std::string logon_str(logon_msg.data(), logon_msg.size());

    session.on_data_received(
        std::span<const char>{logon_str.data(), logon_str.size()});
    REQUIRE(session.state() == SessionState::Active);
    capture.clear();

    // Send heartbeat with stale SendingTime (year 2020)
    auto stale_hb = build_heartbeat("TARGET", "SENDER", 2);
    // The build_heartbeat uses "20260619-12:00:00.000" which is in the past
    // but might be within 120s depending on test timing.
    // Use explicitly stale time:
    MessageAssembler asm2;
    auto stale_msg = fix44::Heartbeat::Builder{}
        .sender_comp_id("TARGET")
        .target_comp_id("SENDER")
        .msg_seq_num(2)
        .sending_time("20200101-00:00:00.000")
        .build(asm2);
    std::string stale_str(stale_msg.data(), stale_msg.size());

    session.on_data_received(
        std::span<const char>{stale_str.data(), stale_str.size()});

    REQUIRE(capture.has_msg_type(msg_type::Reject));
    REQUIRE(capture.has_msg_type(msg_type::Logout));
}

TEST_CASE("QFP-8C: SendingTime check disabled when check_latency=false",
          "[qfp][sendingtime]") {
    TestSession ts;  // check_latency=false by default in TestSession
    ts.establish();

    // Stale time should NOT be rejected
    MessageAssembler asm_;
    auto stale_msg = fix44::Heartbeat::Builder{}
        .sender_comp_id("TARGET")
        .target_comp_id("SENDER")
        .msg_seq_num(2)
        .sending_time("20200101-00:00:00.000")
        .build(asm_);
    std::string stale_str(stale_msg.data(), stale_msg.size());

    ts.feed(stale_str);

    REQUIRE_FALSE(ts.capture.has_msg_type(msg_type::Reject));
    REQUIRE(ts.errors.empty());
}

// ============================================================================
// Category 9: Application Message Dispatch
// ============================================================================

TEST_CASE("QFP-9A: Application message dispatched via callback",
          "[qfp][appmsg]") {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.check_latency = false;
    SessionManager session(config);

    SentCapture capture;
    bool app_msg_received = false;
    char received_msg_type = '\0';

    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool {
        return capture.on_send(d);
    };
    cbs.on_app_message = [&](const ParsedMessage& msg) {
        app_msg_received = true;
        received_msg_type = msg.msg_type();
    };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});
    REQUIRE(session.state() == SessionState::Active);

    auto nos = build_nos("TARGET", "SENDER", 2);
    session.on_data_received(
        std::span<const char>{nos.data(), nos.size()});

    REQUIRE(app_msg_received);
    REQUIRE(received_msg_type == msg_type::NewOrderSingle);
}

TEST_CASE("QFP-9B: Cannot send app message when not Active",
          "[qfp][appmsg]") {
    TestSession ts;
    ts.session.on_connect();

    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ORD001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260619-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.0));

    auto result = ts.session.send_app_message(nos_builder);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == SessionErrorCode::InvalidState);
}

// ============================================================================
// Category 10: Session Statistics
// ============================================================================

TEST_CASE("QFP-10A: Stats track sent/received correctly", "[qfp][stats]") {
    TestSession ts;
    ts.establish();

    auto hb = build_heartbeat("TARGET", "SENDER", 2);
    ts.feed(hb);
    ts.feed(build_heartbeat("TARGET", "SENDER", 3));

    REQUIRE(ts.session.stats().messages_received >= 3);  // logon response + 2 heartbeats
    REQUIRE(ts.session.stats().messages_sent >= 1);
}

TEST_CASE("QFP-10B: Reject stats tracked", "[qfp][stats]") {
    TestSession ts;
    ts.establish();

    auto bad = build_heartbeat("WRONG", "SENDER", 2);
    ts.feed(bad);

    REQUIRE(ts.session.stats().rejects_sent == 1);
}
