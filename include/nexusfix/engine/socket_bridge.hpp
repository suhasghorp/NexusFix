#pragma once

#include <array>
#include <chrono>
#include <cstring>
#include <span>

#include "nexusfix/parser/runtime_parser.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"

namespace nfx {

/// Bridges a TCP socket to a SessionManager via StreamParser message framing.
/// Handles: socket recv -> StreamParser -> session dispatch -> timer tick.
template <size_t BufferSize = 8192>
class SocketBridge {
public:
    explicit SocketBridge(TcpSocket& sock, SessionManager& session) noexcept
        : socket_{sock}
        , session_{session}
        , last_tick_{std::chrono::steady_clock::now()} {}

    /// Poll socket once, feed complete messages to session.
    /// Calls on_timer_tick() at ~100ms intervals.
    /// Returns true if data was received.
    bool poll_once(int timeout_ms = 10) noexcept {
        bool received = false;

        if (socket_.is_connected() && socket_.poll_read(timeout_ms)) {
            auto space = std::span<char>{
                buffer_.data() + buffered_,
                buffer_.size() - buffered_
            };

            if (!space.empty()) {
                auto result = socket_.receive(space);
                if (result.has_value() && *result > 0) {
                    received = true;
                    buffered_ += *result;
                }
            }
        }

        drain_buffered();

        auto now = std::chrono::steady_clock::now();
        if (now - last_tick_ >= tick_interval_) {
            session_.on_timer_tick();
            last_tick_ = now;
        }

        return received;
    }

    /// Reset parser and buffer state (e.g., after reconnect)
    void reset() noexcept {
        parser_.reset();
        buffered_ = 0;
        last_tick_ = std::chrono::steady_clock::now();
    }

private:
    void drain_buffered() noexcept {
        while (buffered_ > 0) {
            auto data = std::span<const char>{buffer_.data(), buffered_};
            size_t consumed = parser_.feed(data);

            while (parser_.has_message()) {
                auto [start, end] = parser_.next_message();
                auto msg_span = data.subspan(start, end - start);
                session_.on_data_received(msg_span);
            }

            if (consumed == 0) break;

            size_t remaining = buffered_ - consumed;
            if (remaining > 0) {
                std::memmove(buffer_.data(), buffer_.data() + consumed, remaining);
            }
            buffered_ = remaining;
        }
    }

    TcpSocket& socket_;
    SessionManager& session_;
    StreamParser parser_;
    std::array<char, BufferSize> buffer_{};
    size_t buffered_{0};
    std::chrono::steady_clock::time_point last_tick_;
    static constexpr std::chrono::milliseconds tick_interval_{100};
};

} // namespace nfx
