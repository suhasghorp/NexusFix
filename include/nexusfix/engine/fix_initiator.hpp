#pragma once

#include <atomic>
#include <chrono>
#include <string>

#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/store/i_message_store.hpp"
#include "nexusfix/transport/tcp_transport.hpp"

namespace nfx {

struct InitiatorConfig {
    std::string host;
    uint16_t port{0};

    std::string sender_comp_id;
    std::string target_comp_id;
    std::string begin_string{"FIX.4.4"};

    int heart_bt_int{30};
    int reconnect_delay_ms{5000};
    int max_reconnect_attempts{3};
    bool auto_reconnect{true};
    bool reset_seq_num_on_logon{false};
    bool validate_comp_ids{true};

    constexpr InitiatorConfig() noexcept = default;
};

/// Client-side FIX engine.
/// Owns transport + session + bridge. Drives heartbeats and reconnection.
class FixInitiator {
public:
    explicit FixInitiator(InitiatorConfig config) noexcept
        : config_{std::move(config)}
        , session_{make_session_config()} {}

    FixInitiator(const FixInitiator&) = delete;
    FixInitiator& operator=(const FixInitiator&) = delete;

    /// Set application callbacks (call before start)
    void set_callbacks(SessionCallbacks callbacks) noexcept {
        user_callbacks_ = std::move(callbacks);
    }

    void set_message_store(store::IMessageStore* store) noexcept {
        session_.set_message_store(store);
    }

    /// Connect + initiate logon, returns immediately.
    /// Use poll() in your own event loop afterward.
    [[nodiscard]] SessionResult<void> start() noexcept {
        wire_callbacks();

        if (!try_connect()) {
            return std::unexpected{SessionError{SessionErrorCode::NotConnected}};
        }

        return session_.initiate_logon();
    }

    /// Single poll cycle: recv/parse/dispatch + timer tick + reconnection.
    /// Returns true if data was received.
    bool poll(int timeout_ms = 10) noexcept {
        if (session_.state() == SessionState::Reconnecting && config_.auto_reconnect) {
            drive_reconnection();
            return false;
        }

        if (!bridge_) return false;

        bool received = bridge_->poll_once(timeout_ms);

        if (!socket_.is_connected() && session_.state() != SessionState::Disconnected
            && session_.state() != SessionState::Reconnecting
            && session_.state() != SessionState::Error) {
            session_.on_disconnect();
        }

        return received;
    }

    /// Poll until a predicate is true or timeout
    bool poll_until(auto pred, int timeout_ms = 2000) noexcept {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            poll(10);
            if (pred()) return true;
        }
        return false;
    }

    /// Initiate graceful logout
    SessionResult<void> stop() noexcept {
        if (session_.state() == SessionState::Active) {
            return session_.initiate_logout();
        }
        return {};
    }

    /// Send an application message via session layer
    template <typename MsgBuilder>
    [[nodiscard]] SessionResult<void> send(MsgBuilder& builder) noexcept {
        return session_.send_app_message(builder);
    }

    [[nodiscard]] SessionState state() const noexcept { return session_.state(); }
    [[nodiscard]] bool is_active() const noexcept { return session_.state() == SessionState::Active; }
    [[nodiscard]] SessionManager& session() noexcept { return session_; }
    [[nodiscard]] const SessionManager& session() const noexcept { return session_; }
    [[nodiscard]] const SessionStats& stats() const noexcept { return session_.stats(); }
    [[nodiscard]] TcpSocket& socket() noexcept { return socket_; }
    [[nodiscard]] int reconnect_count() const noexcept { return reconnect_count_; }

private:
    [[nodiscard]] SessionConfig make_session_config() const noexcept {
        SessionConfig sc;
        sc.sender_comp_id = config_.sender_comp_id;
        sc.target_comp_id = config_.target_comp_id;
        sc.begin_string = config_.begin_string;
        sc.heart_bt_int = config_.heart_bt_int;
        sc.reset_seq_num_on_logon = config_.reset_seq_num_on_logon;
        sc.validate_comp_ids = config_.validate_comp_ids;
        return sc;
    }

    void wire_callbacks() noexcept {
        SessionCallbacks cbs;

        cbs.on_send = [this](std::span<const char> data) -> bool {
            auto result = socket_.send(data);
            return result.has_value() && *result > 0;
        };

        cbs.on_app_message = user_callbacks_.on_app_message;
        cbs.on_state_change = user_callbacks_.on_state_change;
        cbs.on_error = user_callbacks_.on_error;
        cbs.on_logon = user_callbacks_.on_logon;
        cbs.on_logout = user_callbacks_.on_logout;

        session_.set_callbacks(std::move(cbs));
    }

    [[nodiscard]] bool try_connect() noexcept {
        socket_.close();
        auto result = socket_.connect(config_.host, config_.port);
        if (!result.has_value()) return false;

        bridge_ = std::make_unique<SocketBridge<>>(socket_, session_);
        session_.on_connect();
        return true;
    }

    void drive_reconnection() noexcept {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_reconnect_attempt_);

        if (elapsed.count() < config_.reconnect_delay_ms) return;

        if (config_.max_reconnect_attempts > 0 &&
            reconnect_attempts_ >= config_.max_reconnect_attempts) {
            return;
        }

        last_reconnect_attempt_ = now;
        ++reconnect_attempts_;

        if (try_connect()) {
            reconnect_attempts_ = 0;
            ++reconnect_count_;
            (void)session_.initiate_logon();
        }
    }

    InitiatorConfig config_;
    SessionManager session_;
    SessionCallbacks user_callbacks_;
    TcpSocket socket_;
    std::unique_ptr<SocketBridge<>> bridge_;

    int reconnect_attempts_{0};
    int reconnect_count_{0};
    std::chrono::steady_clock::time_point last_reconnect_attempt_{};
};

} // namespace nfx
