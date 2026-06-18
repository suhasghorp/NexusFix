// quick_start.cpp
// NexusFIX Quick Start: Full connect-send-receive flow
//
// Spins up an acceptor (server) and initiator (client) on localhost.
// Demonstrates: logon, send NewOrderSingle, receive ExecutionReport, logout.
//
// Build & run:
//   cmake --build build --target nexusfix_quick_start
//   ./build/bin/examples/nexusfix_quick_start

#include <atomic>
#include <iostream>
#include <string_view>
#include <thread>

#include "nexusfix/engine/fix_initiator.hpp"
#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"
#include "nexusfix/parser/runtime_parser.hpp"
#include "nexusfix/types/field_types.hpp"
#include "nexusfix/types/tag.hpp"

using namespace nfx;

// ============================================================================
// Acceptor (server) thread
// ============================================================================

struct AcceptorThread {
    TcpAcceptor listener;
    std::atomic<bool> running{true};
    std::atomic<bool> logon_done{false};
    std::thread thread;

    uint16_t start() {
        if (!listener.listen(0).has_value()) return 0;
        uint16_t port = listener.local_port();
        thread = std::thread([this] { run(); });
        return port;
    }

    void stop() {
        running.store(false, std::memory_order_release);
        listener.close();
        if (thread.joinable()) thread.join();
    }

private:
    void run() {
        auto fd = listener.accept();
        if (!fd.has_value()) return;

        TcpSocket client{*fd};

        SessionConfig sc;
        sc.sender_comp_id = "SERVER";
        sc.target_comp_id = "CLIENT";
        sc.heart_bt_int = 30;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks cbs;
        cbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        cbs.on_logon = [this]() {
            logon_done.store(true, std::memory_order_release);
            std::cout << "[SERVER] Logon complete\n";
        };
        cbs.on_app_message = [&session](const ParsedMessage& msg) {
            if (msg.msg_type() != msg_type::NewOrderSingle) return;

            std::cout << "[SERVER] Received NOS, sending ExecutionReport\n";

            auto er = fix44::ExecutionReport::Builder{}
                .order_id("ORD-100")
                .exec_id("EXEC-100")
                .exec_type(ExecType::New)
                .ord_status(OrdStatus::New)
                .symbol(msg.get_string(tag::Symbol::value))
                .side(Side::Buy)
                .leaves_qty(Qty::from_int(100))
                .cum_qty(Qty::from_int(0))
                .avg_px(FixedPrice::from_double(0.0))
                .cl_ord_id(msg.get_string(tag::ClOrdID::value))
                .transact_time("20260618-12:00:00.000");
            (void)session.send_app_message(er);
        };
        session.set_callbacks(std::move(cbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(20);
        }
    }
};

// ============================================================================
// Main
// ============================================================================

int main() {
    std::cout << "NexusFIX Quick Start\n\n";

    // 1. Start acceptor on an ephemeral port
    AcceptorThread server;
    uint16_t port = server.start();
    if (port == 0) {
        std::cerr << "Server listen failed\n";
        return 1;
    }
    std::cout << "[SERVER] Listening on port " << port << "\n";

    // 2. Configure and start initiator
    InitiatorConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.sender_comp_id = "CLIENT";
    cfg.target_comp_id = "SERVER";
    cfg.heart_bt_int = 30;
    cfg.validate_comp_ids = false;
    cfg.auto_reconnect = false;

    FixInitiator initiator{cfg};

    std::atomic<bool> er_received{false};

    SessionCallbacks client_cbs;
    client_cbs.on_logon = []() {
        std::cout << "[CLIENT] Logon complete, session active\n";
    };
    client_cbs.on_app_message = [&er_received](const ParsedMessage& msg) {
        if (msg.msg_type() != msg_type::ExecutionReport) return;

        std::cout << "[CLIENT] ExecutionReport received"
                  << "  order_id=" << msg.get_string(tag::OrderID::value)
                  << "  exec_type=" << msg.get_char(tag::ExecType::value)
                  << "  symbol=" << msg.get_string(tag::Symbol::value)
                  << "\n";
        er_received.store(true, std::memory_order_release);
    };
    initiator.set_callbacks(std::move(client_cbs));

    auto start_result = initiator.start();
    if (!start_result) {
        std::cerr << "Initiator connect failed\n";
        server.stop();
        return 1;
    }

    // 3. Wait for logon
    if (!initiator.poll_until([&] { return initiator.is_active(); }, 3000)) {
        std::cerr << "Logon timed out\n";
        server.stop();
        return 1;
    }

    // 4. Send NewOrderSingle
    auto nos = fix44::NewOrderSingle::Builder{}
        .cl_ord_id("ORD001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260618-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.25))
        .time_in_force(TimeInForce::Day);

    auto send_result = initiator.send(nos);
    if (send_result) {
        std::cout << "[CLIENT] Sent NOS: AAPL BUY 100 @ 150.25\n";
    }

    // 5. Poll until ExecutionReport arrives
    initiator.poll_until(
        [&] { return er_received.load(std::memory_order_acquire); }, 3000);

    // 6. Logout and shutdown
    std::cout << "\n";
    (void)initiator.stop();
    initiator.poll_until(
        [&] { return initiator.state() == SessionState::Disconnected; }, 2000);
    server.stop();

    std::cout << "Done.\n";
    return 0;
}
