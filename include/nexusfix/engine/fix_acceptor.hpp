#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/store/i_message_store.hpp"
#include "nexusfix/transport/tcp_transport.hpp"

namespace nfx {

struct AcceptorConfig {
    uint16_t port{0};
    int backlog{128};

    std::string sender_comp_id;
    std::string target_comp_id;
    std::string begin_string{"FIX.4.4"};

    int heart_bt_int{30};
    bool validate_comp_ids{false};

    constexpr AcceptorConfig() noexcept = default;
};

/// Server-side FIX engine (single-session).
/// Listens for one inbound connection and manages its session lifecycle.
class FixAcceptor {
public:
    explicit FixAcceptor(AcceptorConfig config) noexcept
        : config_{std::move(config)} {}

    ~FixAcceptor() { stop(); }

    FixAcceptor(const FixAcceptor&) = delete;
    FixAcceptor& operator=(const FixAcceptor&) = delete;

    /// Set session callbacks (call before listen)
    void set_callbacks(SessionCallbacks callbacks) noexcept {
        user_callbacks_ = std::move(callbacks);
    }

    void set_message_store(store::IMessageStore* store) noexcept {
        message_store_ = store;
    }

    /// Bind and listen. Returns actual port (useful with ephemeral port 0).
    [[nodiscard]] SessionResult<uint16_t> listen() noexcept {
        auto result = acceptor_.listen(config_.port, config_.backlog);
        if (!result.has_value()) {
            return std::unexpected{SessionError{SessionErrorCode::NotConnected}};
        }
        return acceptor_.local_port();
    }

    /// Start accepting in a background thread.
    /// Accepts one connection, runs its session until stop() or disconnect.
    void start_background() noexcept {
        running_.store(true, std::memory_order_release);
        background_thread_ = std::thread([this] { run_loop(); });
    }

    void stop() noexcept {
        running_.store(false, std::memory_order_release);
        acceptor_.close();
        if (background_thread_.joinable()) {
            background_thread_.join();
        }
    }

    [[nodiscard]] uint16_t local_port() const noexcept {
        return acceptor_.local_port();
    }

    [[nodiscard]] bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }

    std::atomic<bool> logon_complete{false};
    std::atomic<bool> logout_complete{false};

private:
    [[nodiscard]] SessionConfig make_session_config() const noexcept {
        SessionConfig sc;
        sc.sender_comp_id = config_.sender_comp_id;
        sc.target_comp_id = config_.target_comp_id;
        sc.begin_string = config_.begin_string;
        sc.heart_bt_int = config_.heart_bt_int;
        sc.validate_comp_ids = config_.validate_comp_ids;
        return sc;
    }

    void run_loop() noexcept {
        while (running_.load(std::memory_order_acquire)) {
            auto accept_result = acceptor_.accept();
            if (!accept_result.has_value()) return;

            logon_complete.store(false, std::memory_order_release);
            logout_complete.store(false, std::memory_order_release);

            TcpSocket client_sock{*accept_result};
            SessionManager session{make_session_config()};

            if (message_store_) {
                session.set_message_store(message_store_);
            }

            SessionCallbacks cbs;
            cbs.on_send = [&client_sock](std::span<const char> data) -> bool {
                auto result = client_sock.send(data);
                return result.has_value() && *result > 0;
            };
            cbs.on_logon = [this]() {
                logon_complete.store(true, std::memory_order_release);
                if (user_callbacks_.on_logon) user_callbacks_.on_logon();
            };
            cbs.on_logout = [this](std::string_view text) {
                logout_complete.store(true, std::memory_order_release);
                if (user_callbacks_.on_logout) user_callbacks_.on_logout(text);
            };
            cbs.on_app_message = [this](const ParsedMessage& msg) {
                if (user_callbacks_.on_app_message) {
                    user_callbacks_.on_app_message(msg);
                }
            };
            cbs.on_state_change = user_callbacks_.on_state_change;
            cbs.on_error = user_callbacks_.on_error;

            session.set_callbacks(std::move(cbs));
            session.on_connect();

            SocketBridge<> bridge{client_sock, session};

            while (running_.load(std::memory_order_acquire) && client_sock.is_connected()) {
                bridge.poll_once(20);
            }

            if (client_sock.is_connected()) {
                session.on_disconnect();
            }
        }
    }

    AcceptorConfig config_;
    TcpAcceptor acceptor_;
    SessionCallbacks user_callbacks_;
    store::IMessageStore* message_store_{nullptr};

    std::atomic<bool> running_{false};
    std::thread background_thread_;
};

} // namespace nfx
