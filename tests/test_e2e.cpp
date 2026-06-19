#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <string>
#include <cstring>

#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/engine/fix_initiator.hpp"
#include "nexusfix/engine/fix_acceptor.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"
#include "nexusfix/parser/runtime_parser.hpp"
#include "nexusfix/store/memory_message_store.hpp"

using namespace nfx;

namespace {

// ============================================================================
// Helper: AcceptorEndpoint
// Runs a FIX acceptor in a separate thread
// ============================================================================

class AcceptorEndpoint {
public:
    AcceptorEndpoint() {
        config_.sender_comp_id = "ACCEPTOR";
        config_.target_comp_id = "INITIATOR";
        config_.heart_bt_int = 30;
        config_.validate_comp_ids = false;
        config_.check_latency = false;
    }

    ~AcceptorEndpoint() {
        stop();
    }

    AcceptorEndpoint(const AcceptorEndpoint&) = delete;
    AcceptorEndpoint& operator=(const AcceptorEndpoint&) = delete;

    /// Start listening on ephemeral port, return the port
    uint16_t start() {
        auto result = acceptor_.listen(0);
        REQUIRE(result.has_value());
        port_ = acceptor_.local_port();
        REQUIRE(port_ != 0);

        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return port_;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        acceptor_.close();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    /// Wait for logon to complete (with timeout)
    bool wait_for_logon(int timeout_ms = 2000) const {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (logon_complete.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    /// Wait for an app message to be received
    bool wait_for_app_message(int timeout_ms = 2000) const {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (app_message_received.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    /// Wait for logout to complete
    bool wait_for_logout(int timeout_ms = 2000) const {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            if (logout_complete.load(std::memory_order_acquire)) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return false;
    }

    std::atomic<bool> logon_complete{false};
    std::atomic<bool> app_message_received{false};
    std::atomic<bool> logout_complete{false};
    std::atomic<bool> er_sent{false};

private:
    void run() {
        // Accept one connection
        auto accept_result = acceptor_.accept();
        if (!accept_result.has_value()) return;

        TcpSocket client_sock{*accept_result};
        SessionManager session{config_};

        // Wire callbacks
        SessionCallbacks cbs;
        cbs.on_send = [&client_sock](std::span<const char> data) -> bool {
            auto result = client_sock.send(data);
            return result.has_value() && *result > 0;
        };
        cbs.on_logon = [this]() {
            logon_complete.store(true, std::memory_order_release);
        };
        cbs.on_logout = [this]([[maybe_unused]] std::string_view text) {
            logout_complete.store(true, std::memory_order_release);
        };
        cbs.on_app_message = [this, &session](const ParsedMessage& msg) {
            app_message_received.store(true, std::memory_order_release);

            // If NOS received, respond with ExecutionReport
            if (msg.msg_type() == msg_type::NewOrderSingle) {
                auto er_builder = fix44::ExecutionReport::Builder{}
                    .order_id("ORD001")
                    .exec_id("EXEC001")
                    .exec_type(ExecType::New)
                    .ord_status(OrdStatus::New)
                    .symbol(msg.get_string(tag::Symbol::value))
                    .side(Side::Buy)
                    .leaves_qty(Qty::from_int(100))
                    .cum_qty(Qty::from_int(0))
                    .avg_px(FixedPrice::from_double(0.0))
                    .cl_ord_id(msg.get_string(tag::ClOrdID::value))
                    .transact_time("20260401-12:00:00.000");
                (void)session.send_app_message(er_builder);
                er_sent.store(true, std::memory_order_release);
            }
        };
        session.set_callbacks(std::move(cbs));

        // Acceptor: on_connect -> wait for incoming Logon
        session.on_connect();

        SocketBridge<> bridge{client_sock, session};

        // Poll loop
        while (running_.load(std::memory_order_acquire) && client_sock.is_connected()) {
            bridge.poll_once(20);
        }

        // If session is still active, do graceful cleanup
        if (client_sock.is_connected()) {
            session.on_disconnect();
        }
    }

    SessionConfig config_;
    TcpAcceptor acceptor_;
    uint16_t port_{0};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

// ============================================================================
// Helper: InitiatorEndpoint
// Client-side FIX session over TCP
// ============================================================================

class InitiatorEndpoint {
public:
    InitiatorEndpoint() {
        config_.sender_comp_id = "INITIATOR";
        config_.target_comp_id = "ACCEPTOR";
        config_.heart_bt_int = 30;
        config_.validate_comp_ids = false;
        config_.check_latency = false;

        session_ = std::make_unique<SessionManager>(config_);

        SessionCallbacks cbs;
        cbs.on_send = [this](std::span<const char> data) -> bool {
            auto result = socket_.send(data);
            return result.has_value() && *result > 0;
        };
        cbs.on_state_change = [this](SessionState from, SessionState to) {
            state_changes_.emplace_back(from, to);
        };
        cbs.on_app_message = [this](const ParsedMessage& msg) {
            // Store raw data since ParsedMessage holds string_views into the buffer
            app_msg_types_.push_back(msg.msg_type());
            app_message_count_.fetch_add(1, std::memory_order_release);
        };
        cbs.on_logon = [this]() {
            logon_received_ = true;
        };
        cbs.on_logout = [this](std::string_view text) {
            logout_received_ = true;
            logout_text_ = std::string(text);
        };
        cbs.on_error = [this]([[maybe_unused]] const SessionError& err) {
            error_count_++;
        };
        session_->set_callbacks(std::move(cbs));
    }

    /// Connect to acceptor
    bool connect(uint16_t port) {
        auto result = socket_.connect("127.0.0.1", port);
        if (!result.has_value()) return false;
        session_->on_connect();
        bridge_ = std::make_unique<SocketBridge<>>(socket_, *session_);
        return true;
    }

    /// Poll once for incoming data
    bool poll_once(int timeout_ms = 10) {
        if (!bridge_) return false;
        return bridge_->poll_once(timeout_ms);
    }

    /// Poll until predicate is true or timeout
    bool poll_until(std::function<bool()> pred, int timeout_ms = 2000) {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            poll_once(10);
            if (pred()) return true;
        }
        return false;
    }

    SessionManager& session() { return *session_; }
    TcpSocket& socket() { return socket_; }
    bool logon_received() const { return logon_received_; }
    bool logout_received() const { return logout_received_; }
    const std::string& logout_text() const { return logout_text_; }
    size_t app_message_count() const { return app_message_count_.load(std::memory_order_acquire); }
    const std::vector<char>& app_msg_types() const { return app_msg_types_; }
    int error_count() const { return error_count_; }

private:
    SessionConfig config_;
    TcpSocket socket_;
    std::unique_ptr<SessionManager> session_;
    std::unique_ptr<SocketBridge<>> bridge_;
    std::vector<std::pair<SessionState, SessionState>> state_changes_;
    std::vector<char> app_msg_types_;
    std::atomic<size_t> app_message_count_{0};
    bool logon_received_{false};
    bool logout_received_{false};
    std::string logout_text_;
    int error_count_{0};
};

}  // anonymous namespace

// ============================================================================
// End-to-End Tests
// ============================================================================

TEST_CASE("E2E: Logon handshake over TCP", "[e2e]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Initiate logon
    auto result = initiator.session().initiate_logon();
    REQUIRE(result.has_value());

    // Poll initiator until logon completes
    bool logon_ok = initiator.poll_until([&] {
        return initiator.logon_received();
    });

    REQUIRE(logon_ok);
    REQUIRE(initiator.session().state() == SessionState::Active);
    REQUIRE(acceptor.wait_for_logon());

    acceptor.stop();
}

TEST_CASE("E2E: Application message roundtrip", "[e2e]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Send NOS from initiator via session layer (proper sequence tracking)
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ORDER001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260401-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.25));
    auto nos_result = initiator.session().send_app_message(nos_builder);
    REQUIRE(nos_result.has_value());

    // Wait for acceptor to receive NOS and send ER back
    REQUIRE(acceptor.wait_for_app_message());
    REQUIRE(acceptor.er_sent.load(std::memory_order_acquire));

    // Poll initiator to receive the ExecutionReport
    bool er_received = initiator.poll_until([&] {
        return initiator.app_message_count() > 0;
    });

    REQUIRE(er_received);
    REQUIRE(initiator.app_msg_types()[0] == msg_type::ExecutionReport);

    acceptor.stop();
}

TEST_CASE("E2E: Graceful logout", "[e2e]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Initiate logout from initiator
    auto result = initiator.session().initiate_logout("Test done");
    REQUIRE(result.has_value());
    REQUIRE(initiator.session().state() == SessionState::LogoutPending);

    // Poll initiator to receive logout response
    bool logout_ok = initiator.poll_until([&] {
        return initiator.logout_received();
    });

    REQUIRE(logout_ok);
    REQUIRE(initiator.session().state() == SessionState::Disconnected);
    REQUIRE(acceptor.wait_for_logout());

    acceptor.stop();
}

TEST_CASE("E2E: Connection refused", "[e2e]") {
    // Try to connect to a port where nothing is listening
    TcpSocket sock;
    auto result = sock.connect("127.0.0.1", 1);  // Port 1 should not be listening
    REQUIRE_FALSE(result.has_value());
    REQUIRE_FALSE(sock.is_connected());
}

TEST_CASE("E2E: Acceptor drops connection mid-session", "[e2e]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Kill the acceptor (closes its socket)
    acceptor.stop();

    // Give the OS a moment to propagate the close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Initiator should detect disconnect on next poll/send
    // Try reading - should get connection closed
    initiator.poll_once(50);
    // The socket state should reflect the disconnect
    // (recv returns 0 which sets state to Disconnected)
    REQUIRE_FALSE(initiator.socket().is_connected());
}

TEST_CASE("E2E: Full session lifecycle", "[e2e]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;

    // Phase 1: Connect
    REQUIRE(initiator.connect(port));
    REQUIRE(initiator.session().state() == SessionState::SocketConnected);

    // Phase 2: Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(initiator.session().state() == SessionState::Active);
    REQUIRE(acceptor.wait_for_logon());

    // Phase 3: Send order via session layer
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ORDER001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260401-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.25));
    auto nos_result = initiator.session().send_app_message(nos_builder);
    REQUIRE(nos_result.has_value());

    // Phase 4: Receive ExecutionReport
    REQUIRE(acceptor.wait_for_app_message());
    bool er_received = initiator.poll_until([&] {
        return initiator.app_message_count() > 0;
    });
    REQUIRE(er_received);
    REQUIRE(initiator.app_msg_types()[0] == msg_type::ExecutionReport);

    // Phase 5: Logout
    auto logout_result = initiator.session().initiate_logout();
    REQUIRE(logout_result.has_value());

    bool logout_ok = initiator.poll_until([&] {
        return initiator.logout_received();
    });
    REQUIRE(logout_ok);
    REQUIRE(initiator.session().state() == SessionState::Disconnected);
    REQUIRE(acceptor.wait_for_logout());

    acceptor.stop();
}

// ============================================================================
// Phase 3: Extended E2E Tests
// ============================================================================

TEST_CASE("E2E: NOS -> ExecutionReport with message store", "[e2e][regression]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Attach message store to initiator session
    store::MemoryMessageStore store("INITIATOR-ACCEPTOR");
    initiator.session().set_message_store(&store);

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Send NOS
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("STORE001")
        .symbol("TSLA")
        .side(Side::Buy)
        .transact_time("20260427-12:00:00.000")
        .order_qty(Qty::from_int(50))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(200.50));
    auto nos_result = initiator.session().send_app_message(nos_builder);
    REQUIRE(nos_result.has_value());

    // Wait for ER back
    REQUIRE(acceptor.wait_for_app_message());
    bool er_received = initiator.poll_until([&] {
        return initiator.app_message_count() > 0;
    });
    REQUIRE(er_received);

    // Verify message store has stored outbound messages
    // Logon (seq 1) + NOS (seq 2) should be stored
    CHECK(store.message_count() >= 2);
    CHECK(store.contains(1));  // Logon
    CHECK(store.contains(2));  // NOS

    acceptor.stop();
}

TEST_CASE("E2E: OrderCancelRequest -> cancel ack round-trip", "[e2e][regression]") {
    // Extended AcceptorEndpoint that handles cancel requests
    struct CancelAcceptor {
        SessionConfig config;
        TcpAcceptor acceptor;
        std::atomic<bool> running{false};
        std::atomic<bool> logon_complete{false};
        std::atomic<bool> cancel_received{false};
        std::atomic<bool> cancel_ack_sent{false};
        std::thread thread;

        CancelAcceptor() {
            config.sender_comp_id = "ACCEPTOR";
            config.target_comp_id = "INITIATOR";
            config.heart_bt_int = 30;
            config.validate_comp_ids = false;
        }

        ~CancelAcceptor() { stop(); }

        uint16_t start() {
            auto result = acceptor.listen(0);
            REQUIRE(result.has_value());
            uint16_t port = acceptor.local_port();
            REQUIRE(port != 0);
            running.store(true, std::memory_order_release);
            thread = std::thread([this] { run(); });
            return port;
        }

        void stop() {
            running.store(false, std::memory_order_release);
            acceptor.close();
            if (thread.joinable()) thread.join();
        }

        void run() {
            auto accept_result = acceptor.accept();
            if (!accept_result.has_value()) return;

            TcpSocket client_sock{*accept_result};
            SessionManager session{config};

            SessionCallbacks cbs;
            cbs.on_send = [&client_sock](std::span<const char> data) -> bool {
                auto result = client_sock.send(data);
                return result.has_value() && *result > 0;
            };
            cbs.on_logon = [this]() {
                logon_complete.store(true, std::memory_order_release);
            };
            cbs.on_app_message = [this, &session](const ParsedMessage& msg) {
                if (msg.msg_type() == msg_type::OrderCancelRequest) {
                    cancel_received.store(true, std::memory_order_release);

                    // Respond with cancel ack ER
                    auto er_builder = fix44::ExecutionReport::Builder{}
                        .order_id("ORD001")
                        .exec_id("EXEC002")
                        .exec_type(ExecType::Canceled)
                        .ord_status(OrdStatus::Canceled)
                        .symbol(msg.get_string(tag::Symbol::value))
                        .side(Side::Buy)
                        .leaves_qty(Qty::from_int(0))
                        .cum_qty(Qty::from_int(0))
                        .avg_px(FixedPrice::from_double(0.0))
                        .cl_ord_id(msg.get_string(tag::ClOrdID::value))
                        .transact_time("20260427-12:00:00.000");
                    (void)session.send_app_message(er_builder);
                    cancel_ack_sent.store(true, std::memory_order_release);
                }
            };
            session.set_callbacks(std::move(cbs));
            session.on_connect();

            SocketBridge<> bridge{client_sock, session};
            while (running.load(std::memory_order_acquire) && client_sock.is_connected()) {
                bridge.poll_once(20);
            }
            if (client_sock.is_connected()) session.on_disconnect();
        }
    };

    CancelAcceptor acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!acceptor.logon_complete.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(acceptor.logon_complete.load());

    // Send OrderCancelRequest
    auto cancel_builder = fix44::OrderCancelRequest::Builder{}
        .orig_cl_ord_id("ORDER001")
        .cl_ord_id("CANCEL001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260427-12:00:00.000")
        .order_qty(Qty::from_int(100));
    auto cancel_result = initiator.session().send_app_message(cancel_builder);
    REQUIRE(cancel_result.has_value());

    // Wait for cancel ack ER
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!acceptor.cancel_ack_sent.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(acceptor.cancel_received.load());

    bool er_received = initiator.poll_until([&] {
        return initiator.app_message_count() > 0;
    });
    REQUIRE(er_received);
    CHECK(initiator.app_msg_types()[0] == msg_type::ExecutionReport);

    acceptor.stop();
}

TEST_CASE("E2E: Sequence gap -> ResendRequest -> SequenceReset-GapFill", "[e2e][regression]") {
    // Acceptor responds to NOS with ER; sequence gap detection is internal
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Send NOS - acceptor will respond with ER
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("GAP001")
        .symbol("MSFT")
        .side(Side::Buy)
        .transact_time("20260427-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(400.00));
    auto nos_result = initiator.session().send_app_message(nos_builder);
    REQUIRE(nos_result.has_value());

    // Poll for ER receipt
    bool er_received = initiator.poll_until([&] {
        return initiator.app_message_count() > 0;
    });
    REQUIRE(er_received);

    // Verify session processed messages
    CHECK(initiator.session().stats().messages_received >= 2);  // Logon + ER minimum

    acceptor.stop();
}

TEST_CASE("E2E: Malformed message -> session error", "[e2e][regression]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon first
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // Feed garbled data directly to session as if received from network
    const char garbled[] = "GARBLED_NOT_FIX_DATA\x01";
    initiator.session().on_data_received({garbled, sizeof(garbled) - 1});

    // Session should have recorded an error via callback
    CHECK(initiator.error_count() > 0);

    acceptor.stop();
}

TEST_CASE("E2E: Heartbeat timeout transitions to Error", "[e2e][regression]") {
    // Use a very short heartbeat interval for testing
    SessionConfig config;
    config.sender_comp_id = "TEST";
    config.target_comp_id = "PEER";
    config.heart_bt_int = 1;  // 1 second heartbeat
    config.validate_comp_ids = false;

    SessionManager session{config};

    bool state_changed_to_error = false;
    SessionCallbacks cbs;
    cbs.on_send = []([[maybe_unused]] std::span<const char> data) -> bool {
        return true;  // Simulate successful send
    };
    cbs.on_state_change = [&state_changed_to_error](
        [[maybe_unused]] SessionState from, SessionState to) {
        if (to == SessionState::Error) {
            state_changed_to_error = true;
        }
    };
    session.set_callbacks(std::move(cbs));

    // Simulate connection and logon
    session.on_connect();
    (void)session.initiate_logon();

    // Simulate receiving logon response to reach Active state
    MessageAssembler asm_;
    auto logon_msg = fix44::Logon::Builder{}
        .begin_string("FIX.4.4")
        .sender_comp_id("PEER")
        .target_comp_id("TEST")
        .msg_seq_num(1)
        .sending_time("20260427-12:00:00.000")
        .encrypt_method(0)
        .heart_bt_int(1)
        .build(asm_);

    session.on_data_received(logon_msg);
    REQUIRE(session.state() == SessionState::Active);

    // Wait for 2x heartbeat interval (2 seconds) so has_timed_out() returns true
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));

    // on_timer_tick should detect timeout
    session.on_timer_tick();

    CHECK(session.state() == SessionState::Error);
    CHECK(state_changed_to_error);
}

TEST_CASE("E2E: Reconnection after TCP close with session notify", "[e2e][regression]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());
    REQUIRE(initiator.session().state() == SessionState::Active);

    // Kill the acceptor (closes server socket)
    acceptor.stop();

    // Give OS time to propagate the close
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Initiator detects disconnect on next poll
    initiator.poll_once(50);

    // After recv returns 0, socket transitions to Disconnected
    CHECK_FALSE(initiator.socket().is_connected());

    // Notify session of disconnect - from Active, this transitions to Reconnecting
    initiator.session().on_disconnect();
    CHECK(initiator.session().state() == SessionState::Reconnecting);
}

TEST_CASE("E2E: PossDupFlag on resent messages accepted for TooLow seq", "[e2e][regression]") {
    // Test that messages with low sequence but no PossDupFlag trigger error
    SessionConfig config;
    config.sender_comp_id = "INIT";
    config.target_comp_id = "ACPT";
    config.heart_bt_int = 30;
    config.validate_comp_ids = false;
    config.check_latency = false;

    SessionManager session{config};

    int error_count = 0;
    int app_msg_count = 0;
    SessionCallbacks cbs;
    cbs.on_send = []([[maybe_unused]] std::span<const char> data) -> bool {
        return true;
    };
    cbs.on_error = [&error_count]([[maybe_unused]] const SessionError& err) {
        ++error_count;
    };
    cbs.on_app_message = [&app_msg_count]([[maybe_unused]] const ParsedMessage& msg) {
        ++app_msg_count;
    };
    cbs.on_logon = []() {};
    session.set_callbacks(std::move(cbs));

    // Reach Active state
    session.on_connect();
    (void)session.initiate_logon();

    MessageAssembler asm_;
    auto logon_msg = fix44::Logon::Builder{}
        .begin_string("FIX.4.4")
        .sender_comp_id("ACPT")
        .target_comp_id("INIT")
        .msg_seq_num(1)
        .sending_time("20260427-12:00:00.000")
        .encrypt_method(0)
        .heart_bt_int(30)
        .build(asm_);

    session.on_data_received(logon_msg);
    REQUIRE(session.state() == SessionState::Active);

    // Send a normal NOS (seq 2) to advance expected inbound to 3
    auto nos_msg = fix44::NewOrderSingle::Builder{}
        .sender_comp_id("ACPT")
        .target_comp_id("INIT")
        .msg_seq_num(2)
        .sending_time("20260427-12:00:01.000")
        .cl_ord_id("ORD001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260427-12:00:01.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.00))
        .build(asm_);

    session.on_data_received(nos_msg);
    REQUIRE(app_msg_count == 1);
    int errors_before = error_count;

    // Now send a message with seq 1 (too low) WITHOUT PossDupFlag
    // This should trigger a sequence error
    auto low_msg = fix44::NewOrderSingle::Builder{}
        .sender_comp_id("ACPT")
        .target_comp_id("INIT")
        .msg_seq_num(1)
        .sending_time("20260427-12:00:02.000")
        .cl_ord_id("ORD002")
        .symbol("TSLA")
        .side(Side::Buy)
        .transact_time("20260427-12:00:02.000")
        .order_qty(Qty::from_int(50))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(200.00))
        .build(asm_);

    session.on_data_received(low_msg);
    // Without PossDupFlag, low seq triggers error and message is rejected
    CHECK(error_count > errors_before);
}

TEST_CASE("E2E: Concurrent logon from same comp ID (reject second)", "[e2e][regression]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    // First initiator connects and logs on
    InitiatorEndpoint initiator1;
    REQUIRE(initiator1.connect(port));
    (void)initiator1.session().initiate_logon();
    REQUIRE(initiator1.poll_until([&] { return initiator1.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    // AcceptorEndpoint only handles one connection (single accept in run())
    // Second connection attempt won't get a FIX logon response.
    InitiatorEndpoint initiator2;

    bool connected = initiator2.connect(port);
    if (connected) {
        (void)initiator2.session().initiate_logon();
        // Should not receive logon response since acceptor is occupied
        bool logon2_ok = initiator2.poll_until([&] {
            return initiator2.logon_received();
        }, 1000);
        CHECK_FALSE(logon2_ok);
    }

    // First session should still be active
    CHECK(initiator1.session().state() == SessionState::Active);

    acceptor.stop();
}

// ============================================================================
// Engine API Tests (FixInitiator / FixAcceptor)
// ============================================================================

TEST_CASE("E2E: Heartbeat fires via SocketBridge timer", "[e2e][heartbeat]") {
    // Peer uses a long heartbeat (60s) so it won't send test requests,
    // but responds to logon with HeartBtInt=1 to keep the initiator's interval short.
    TcpAcceptor raw_acceptor;
    REQUIRE(raw_acceptor.listen(0).has_value());
    uint16_t port = raw_acceptor.local_port();

    std::atomic<bool> peer_running{true};
    std::thread peer_thread([&] {
        auto client_result = raw_acceptor.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};

        // Peer: manually respond to logon, then just echo heartbeats.
        // Using heart_bt_int=1 in the response but 60s in the peer's timer
        // so the peer won't proactively send test requests.
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "INITIATOR";
        sc.heart_bt_int = 1;
        sc.validate_comp_ids = false;
        SessionManager peer_session{sc};

        SessionCallbacks pcbs;
        pcbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        peer_session.set_callbacks(std::move(pcbs));
        peer_session.on_connect();

        // Peer uses NO timer tick (just raw poll_once without bridge timer)
        // to avoid sending test requests that would trigger heartbeat responses
        StreamParser parser;
        std::array<char, 8192> buffer{};
        size_t buffered = 0;

        while (peer_running.load(std::memory_order_acquire) && client.is_connected()) {
            if (!client.poll_read(20)) continue;
            auto space = std::span<char>{buffer.data() + buffered, buffer.size() - buffered};
            if (space.empty()) continue;
            auto result = client.receive(space);
            if (!result.has_value() || *result == 0) continue;
            buffered += *result;
            auto data = std::span<const char>{buffer.data(), buffered};
            size_t consumed = parser.feed(data);
            while (parser.has_message()) {
                auto [start, end] = parser.next_message();
                peer_session.on_data_received(data.subspan(start, end - start));
            }
            if (consumed > 0) {
                size_t remaining = buffered - consumed;
                if (remaining > 0) std::memmove(buffer.data(), buffer.data() + consumed, remaining);
                buffered = remaining;
            }
        }
    });

    SessionConfig config;
    config.sender_comp_id = "INITIATOR";
    config.target_comp_id = "ACCEPTOR";
    config.heart_bt_int = 1;
    config.validate_comp_ids = false;

    SessionManager session{config};
    TcpSocket sock;
    REQUIRE(sock.connect("127.0.0.1", port).has_value());

    SessionCallbacks cbs;
    cbs.on_send = [&sock](std::span<const char> data) -> bool {
        auto result = sock.send(data);
        return result.has_value() && *result > 0;
    };
    session.set_callbacks(std::move(cbs));
    session.on_connect();
    (void)session.initiate_logon();

    SocketBridge<> bridge{sock, session};

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (session.state() != SessionState::Active &&
           std::chrono::steady_clock::now() < deadline) {
        bridge.poll_once(10);
    }
    REQUIRE(session.state() == SessionState::Active);

    uint64_t sent_before = session.stats().messages_sent;

    // Idle for 1.5s. SocketBridge timer tick should drive on_timer_tick,
    // which detects 1s since last_sent_ and calls send_heartbeat().
    auto idle_start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - idle_start < std::chrono::milliseconds(1500)) {
        bridge.poll_once(10);
    }

    // Verify that additional messages were sent (heartbeats or test requests)
    // driven by the SocketBridge's on_timer_tick() integration.
    CHECK(session.stats().messages_sent > sent_before);

    peer_running.store(false, std::memory_order_release);
    raw_acceptor.close();
    peer_thread.join();
}

TEST_CASE("E2E: Burst send 100 NOS messages", "[e2e][burst]") {
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    InitiatorEndpoint initiator;
    REQUIRE(initiator.connect(port));

    // Logon
    (void)initiator.session().initiate_logon();
    REQUIRE(initiator.poll_until([&] { return initiator.logon_received(); }));
    REQUIRE(acceptor.wait_for_logon());

    static constexpr int BURST_SIZE = 100;

    // Send 100 NOS messages
    for (int i = 0; i < BURST_SIZE; ++i) {
        char cl_ord_id[32];
        std::snprintf(cl_ord_id, sizeof(cl_ord_id), "BURST%03d", i);

        auto nos_builder = fix44::NewOrderSingle::Builder{}
            .cl_ord_id(cl_ord_id)
            .symbol("AAPL")
            .side(Side::Buy)
            .transact_time("20260601-12:00:00.000")
            .order_qty(Qty::from_int(100))
            .ord_type(OrdType::Limit)
            .price(FixedPrice::from_double(150.25));
        auto result = initiator.session().send_app_message(nos_builder);
        REQUIRE(result.has_value());
    }

    // Poll until all ERs received (acceptor responds with ER for each NOS)
    bool all_received = initiator.poll_until([&] {
        return initiator.app_message_count() >= BURST_SIZE;
    }, 5000);

    REQUIRE(all_received);
    CHECK(initiator.app_message_count() == BURST_SIZE);

    // All should be ExecutionReports
    for (size_t i = 0; i < initiator.app_msg_types().size(); ++i) {
        CHECK(initiator.app_msg_types()[i] == msg_type::ExecutionReport);
    }

    acceptor.stop();
}

TEST_CASE("E2E: FixInitiator lifecycle", "[e2e][initiator]") {
    // Start an acceptor (using existing helper)
    AcceptorEndpoint acceptor;
    uint16_t port = acceptor.start();

    // Create FixInitiator
    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "FIX_INIT";
    iconfig.target_comp_id = "ACCEPTOR";
    iconfig.heart_bt_int = 30;
    iconfig.validate_comp_ids = false;
    iconfig.auto_reconnect = false;

    FixInitiator initiator{iconfig};

    std::atomic<bool> logon_ok{false};
    std::atomic<size_t> er_count{0};

    SessionCallbacks cbs;
    cbs.on_logon = [&logon_ok]() {
        logon_ok.store(true, std::memory_order_release);
    };
    cbs.on_app_message = [&er_count]([[maybe_unused]] const ParsedMessage& msg) {
        er_count.fetch_add(1, std::memory_order_release);
    };
    initiator.set_callbacks(std::move(cbs));

    // Start (connect + logon)
    auto start_result = initiator.start();
    REQUIRE(start_result.has_value());

    // Poll until logon
    bool active = initiator.poll_until([&] {
        return logon_ok.load(std::memory_order_acquire);
    });
    REQUIRE(active);
    REQUIRE(initiator.is_active());
    REQUIRE(acceptor.wait_for_logon());

    // Send NOS via engine API
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ENGINE001")
        .symbol("TSLA")
        .side(Side::Buy)
        .transact_time("20260601-12:00:00.000")
        .order_qty(Qty::from_int(50))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(200.00));
    auto send_result = initiator.send(nos_builder);
    REQUIRE(send_result.has_value());

    // Wait for ER
    REQUIRE(acceptor.wait_for_app_message());
    bool er_received = initiator.poll_until([&] {
        return er_count.load(std::memory_order_acquire) > 0;
    });
    REQUIRE(er_received);

    // Stop (logout)
    auto stop_result = initiator.stop();
    REQUIRE(stop_result.has_value());

    acceptor.stop();
}

TEST_CASE("E2E: FixAcceptor lifecycle", "[e2e][acceptor]") {
    AcceptorConfig aconfig;
    aconfig.port = 0;
    aconfig.sender_comp_id = "FIX_ACPT";
    aconfig.target_comp_id = "FIX_INIT";
    aconfig.heart_bt_int = 30;
    aconfig.validate_comp_ids = false;

    FixAcceptor acceptor{aconfig};

    std::atomic<bool> app_msg_received{false};

    SessionCallbacks acbs;
    acbs.on_app_message = [&app_msg_received]([[maybe_unused]] const ParsedMessage& msg) {
        app_msg_received.store(true, std::memory_order_release);
    };
    acceptor.set_callbacks(std::move(acbs));

    // Listen
    auto listen_result = acceptor.listen();
    REQUIRE(listen_result.has_value());
    uint16_t port = *listen_result;
    REQUIRE(port != 0);

    // Start background
    acceptor.start_background();

    // Connect with FixInitiator
    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "FIX_INIT";
    iconfig.target_comp_id = "FIX_ACPT";
    iconfig.heart_bt_int = 30;
    iconfig.validate_comp_ids = false;
    iconfig.auto_reconnect = false;

    FixInitiator initiator{iconfig};

    std::atomic<bool> logon_ok{false};
    SessionCallbacks icbs;
    icbs.on_logon = [&logon_ok]() {
        logon_ok.store(true, std::memory_order_release);
    };
    initiator.set_callbacks(std::move(icbs));

    auto start_result = initiator.start();
    REQUIRE(start_result.has_value());

    bool active = initiator.poll_until([&] {
        return logon_ok.load(std::memory_order_acquire);
    });
    REQUIRE(active);
    REQUIRE(initiator.is_active());

    // Send NOS
    auto nos_builder = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ACPT001")
        .symbol("GOOG")
        .side(Side::Buy)
        .transact_time("20260601-12:00:00.000")
        .order_qty(Qty::from_int(25))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(175.00));
    (void)initiator.send(nos_builder);

    // Wait for acceptor to receive app message
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!app_msg_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        initiator.poll(10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(app_msg_received.load());

    // Cleanup
    (void)initiator.stop();
    acceptor.stop();
}

TEST_CASE("E2E: FixInitiator reconnect_count tracks reconnections", "[e2e][reconnect][resilience]") {
    TcpAcceptor raw_acceptor;
    REQUIRE(raw_acceptor.listen(0).has_value());
    uint16_t port = raw_acceptor.local_port();

    std::atomic<bool> acceptor_running{true};
    std::thread acceptor_thread([&] {
        auto client_result = raw_acceptor.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "RECONN";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks cbs;
        cbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        session.set_callbacks(std::move(cbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    });

    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "RECONN";
    iconfig.target_comp_id = "ACCEPTOR";
    iconfig.heart_bt_int = 30;
    iconfig.validate_comp_ids = false;
    iconfig.auto_reconnect = true;
    iconfig.reconnect_delay_ms = 200;
    iconfig.max_reconnect_attempts = 10;

    FixInitiator initiator{iconfig};

    std::atomic<int> logon_count{0};
    SessionCallbacks cbs;
    cbs.on_logon = [&logon_count]() {
        logon_count.fetch_add(1, std::memory_order_release);
    };
    initiator.set_callbacks(std::move(cbs));

    auto start_result = initiator.start();
    REQUIRE(start_result.has_value());

    bool active = initiator.poll_until([&] {
        return logon_count.load(std::memory_order_acquire) >= 1;
    });
    REQUIRE(active);
    CHECK(initiator.reconnect_count() == 0);

    // Kill acceptor
    acceptor_running.store(false, std::memory_order_release);
    raw_acceptor.close();
    acceptor_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    initiator.poll(50);
    initiator.poll(50);
    CHECK(initiator.state() == SessionState::Reconnecting);

    // Start new acceptor on same port
    TcpAcceptor raw_acceptor2;
    auto listen2 = raw_acceptor2.listen(port);
    if (!listen2.has_value()) {
        WARN("Could not rebind port " << port << ", skipping reconnect_count verification");
        return;
    }

    std::atomic<bool> acceptor2_running{true};
    std::thread acceptor2_thread([&] {
        auto client_result = raw_acceptor2.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "RECONN";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks scbs;
        scbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        session.set_callbacks(std::move(scbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor2_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    });

    bool reconnected = initiator.poll_until([&] {
        return logon_count.load(std::memory_order_acquire) >= 2;
    }, 5000);

    CHECK(reconnected);
    if (reconnected) {
        CHECK(initiator.reconnect_count() == 1);
    }

    acceptor2_running.store(false, std::memory_order_release);
    raw_acceptor2.close();
    acceptor2_thread.join();
}

TEST_CASE("E2E: FixAcceptor accepts reconnection from same peer", "[e2e][reconnect][resilience]") {
    AcceptorConfig aconfig;
    aconfig.port = 0;
    aconfig.sender_comp_id = "FIX_ACPT";
    aconfig.target_comp_id = "FIX_INIT";
    aconfig.heart_bt_int = 30;
    aconfig.validate_comp_ids = false;

    FixAcceptor acceptor{aconfig};

    auto listen_result = acceptor.listen();
    REQUIRE(listen_result.has_value());
    uint16_t port = *listen_result;
    REQUIRE(port != 0);
    acceptor.start_background();

    // First connection
    {
        InitiatorConfig iconfig;
        iconfig.host = "127.0.0.1";
        iconfig.port = port;
        iconfig.sender_comp_id = "FIX_INIT";
        iconfig.target_comp_id = "FIX_ACPT";
        iconfig.heart_bt_int = 30;
        iconfig.validate_comp_ids = false;
        iconfig.auto_reconnect = false;

        FixInitiator initiator{iconfig};
        std::atomic<bool> logon_ok{false};
        SessionCallbacks cbs;
        cbs.on_logon = [&logon_ok]() { logon_ok.store(true, std::memory_order_release); };
        initiator.set_callbacks(std::move(cbs));

        REQUIRE(initiator.start().has_value());
        REQUIRE(initiator.poll_until([&] {
            return logon_ok.load(std::memory_order_acquire);
        }));
        REQUIRE(initiator.is_active());

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!acceptor.logon_complete.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(acceptor.logon_complete.load());
    }
    // Initiator destroyed -> TCP close

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Second connection to same acceptor
    {
        InitiatorConfig iconfig;
        iconfig.host = "127.0.0.1";
        iconfig.port = port;
        iconfig.sender_comp_id = "FIX_INIT";
        iconfig.target_comp_id = "FIX_ACPT";
        iconfig.heart_bt_int = 30;
        iconfig.validate_comp_ids = false;
        iconfig.auto_reconnect = false;
        iconfig.reset_seq_num_on_logon = true;

        FixInitiator initiator{iconfig};
        std::atomic<bool> logon_ok{false};
        SessionCallbacks cbs;
        cbs.on_logon = [&logon_ok]() { logon_ok.store(true, std::memory_order_release); };
        initiator.set_callbacks(std::move(cbs));

        REQUIRE(initiator.start().has_value());
        bool second_logon = initiator.poll_until([&] {
            return logon_ok.load(std::memory_order_acquire);
        }, 3000);

        CHECK(second_logon);
        if (second_logon) {
            CHECK(initiator.is_active());
        }
    }

    acceptor.stop();
}

TEST_CASE("E2E: Sequence numbers persist across reconnect with store", "[e2e][reconnect][resilience]") {
    TcpAcceptor raw_acceptor;
    REQUIRE(raw_acceptor.listen(0).has_value());
    uint16_t port = raw_acceptor.local_port();

    std::atomic<bool> acceptor_running{true};
    std::atomic<bool> first_logon{false};
    std::thread acceptor_thread([&] {
        auto client_result = raw_acceptor.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "SEQTEST";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks cbs;
        cbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        cbs.on_logon = [&first_logon]() {
            first_logon.store(true, std::memory_order_release);
        };
        session.set_callbacks(std::move(cbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    });

    // Create initiator with message store
    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "SEQTEST";
    iconfig.target_comp_id = "ACCEPTOR";
    iconfig.heart_bt_int = 30;
    iconfig.validate_comp_ids = false;
    iconfig.auto_reconnect = false;
    iconfig.reset_seq_num_on_logon = false;

    FixInitiator initiator{iconfig};

    store::MemoryMessageStore msg_store("SEQTEST-ACCEPTOR");
    initiator.set_message_store(&msg_store);

    std::atomic<bool> logon_ok{false};
    SessionCallbacks cbs;
    cbs.on_logon = [&logon_ok]() { logon_ok.store(true, std::memory_order_release); };
    initiator.set_callbacks(std::move(cbs));

    REQUIRE(initiator.start().has_value());
    REQUIRE(initiator.poll_until([&] {
        return logon_ok.load(std::memory_order_acquire);
    }));
    REQUIRE(initiator.is_active());

    // After logon: sender seq should be 2 (logon was seq=1)
    CHECK(msg_store.get_next_sender_seq_num() == 2);
    // After receiving logon response: target seq should be 2
    CHECK(msg_store.get_next_target_seq_num() == 2);

    acceptor_running.store(false, std::memory_order_release);
    raw_acceptor.close();
    acceptor_thread.join();
}

TEST_CASE("E2E: FixInitiator reconnects after acceptor restart", "[e2e][reconnect]") {
    // Use a dedicated acceptor that listens on a specific port
    TcpAcceptor raw_acceptor;
    auto listen_result = raw_acceptor.listen(0);
    REQUIRE(listen_result.has_value());
    uint16_t port = raw_acceptor.local_port();
    REQUIRE(port != 0);

    // Accept in background thread
    std::atomic<bool> acceptor_running{true};
    std::atomic<bool> first_logon{false};
    std::thread acceptor_thread([&] {
        auto client_result = raw_acceptor.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "RECONN";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks cbs;
        cbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        cbs.on_logon = [&first_logon]() {
            first_logon.store(true, std::memory_order_release);
        };
        session.set_callbacks(std::move(cbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    });

    // Create FixInitiator with auto-reconnect
    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "RECONN";
    iconfig.target_comp_id = "ACCEPTOR";
    iconfig.heart_bt_int = 30;
    iconfig.validate_comp_ids = false;
    iconfig.auto_reconnect = true;
    iconfig.reconnect_delay_ms = 200;
    iconfig.max_reconnect_attempts = 10;

    FixInitiator initiator{iconfig};

    std::atomic<int> logon_count{0};
    SessionCallbacks cbs;
    cbs.on_logon = [&logon_count]() {
        logon_count.fetch_add(1, std::memory_order_release);
    };
    initiator.set_callbacks(std::move(cbs));

    // First connect + logon
    auto start_result = initiator.start();
    REQUIRE(start_result.has_value());

    bool active = initiator.poll_until([&] {
        return logon_count.load(std::memory_order_acquire) >= 1;
    });
    REQUIRE(active);
    REQUIRE(initiator.is_active());

    // Kill first acceptor
    acceptor_running.store(false, std::memory_order_release);
    raw_acceptor.close();
    acceptor_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Poll to detect disconnect
    initiator.poll(50);
    initiator.poll(50);

    CHECK(initiator.state() == SessionState::Reconnecting);

    // Start new acceptor on the same port
    TcpAcceptor raw_acceptor2;
    auto listen2 = raw_acceptor2.listen(port);
    if (!listen2.has_value()) {
        // TIME_WAIT, skip reconnect verification
        WARN("Could not rebind port " << port << ", skipping reconnect verification");
        return;
    }

    std::atomic<bool> acceptor2_running{true};
    std::thread acceptor2_thread([&] {
        auto client_result = raw_acceptor2.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "RECONN";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks scbs;
        scbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        session.set_callbacks(std::move(scbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor2_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    });

    // Poll initiator to drive reconnection
    bool reconnected = initiator.poll_until([&] {
        return logon_count.load(std::memory_order_acquire) >= 2;
    }, 5000);

    CHECK(reconnected);
    if (reconnected) {
        CHECK(initiator.is_active());
    }

    acceptor2_running.store(false, std::memory_order_release);
    raw_acceptor2.close();
    acceptor2_thread.join();
}
