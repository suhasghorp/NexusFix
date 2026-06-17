// pipeline_bench.cpp
// NexusFIX End-to-End Pipeline Latency Benchmark
// Measures round-trip: Initiator send NOS -> Acceptor recv -> Acceptor send ER -> Initiator recv

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <thread>
#include <vector>
#include <algorithm>
#include <cmath>

#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"
#include "benchmark_utils.hpp"

using namespace nfx;

static constexpr int WARMUP_ITERATIONS = 1000;
static constexpr int BENCH_ITERATIONS  = 10000;

struct PipelineStats {
    double min_us, max_us, mean_us, stddev_us;
    double p50_us, p90_us, p99_us, p999_us;
    size_t count;
};

static PipelineStats compute_stats(std::vector<double>& samples) {
    PipelineStats s{};
    s.count = samples.size();
    if (samples.empty()) return s;

    std::sort(samples.begin(), samples.end());

    s.min_us = samples.front();
    s.max_us = samples.back();

    double sum = 0;
    for (auto v : samples) sum += v;
    s.mean_us = sum / static_cast<double>(s.count);

    double var = 0;
    for (auto v : samples) {
        double d = v - s.mean_us;
        var += d * d;
    }
    s.stddev_us = std::sqrt(var / static_cast<double>(s.count));

    auto pct = [&](double p) -> double {
        size_t idx = static_cast<size_t>(p * static_cast<double>(s.count - 1));
        return samples[idx];
    };
    s.p50_us  = pct(0.50);
    s.p90_us  = pct(0.90);
    s.p99_us  = pct(0.99);
    s.p999_us = pct(0.999);

    return s;
}

static void print_stats(const char* label, const PipelineStats& s) {
    std::cout << "\n=== " << label << " (" << s.count << " iterations) ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Min:     " << std::setw(10) << s.min_us  << " us\n";
    std::cout << "  P50:     " << std::setw(10) << s.p50_us  << " us\n";
    std::cout << "  P90:     " << std::setw(10) << s.p90_us  << " us\n";
    std::cout << "  P99:     " << std::setw(10) << s.p99_us  << " us\n";
    std::cout << "  P99.9:   " << std::setw(10) << s.p999_us << " us\n";
    std::cout << "  Max:     " << std::setw(10) << s.max_us  << " us\n";
    std::cout << "  Mean:    " << std::setw(10) << s.mean_us << " us\n";
    std::cout << "  Stddev:  " << std::setw(10) << s.stddev_us << " us\n";
}

int main() {
    std::cout << "NexusFIX End-to-End Pipeline Benchmark\n";
    std::cout << "======================================\n";
    std::cout << "Warmup: " << WARMUP_ITERATIONS << " iterations\n";
    std::cout << "Bench:  " << BENCH_ITERATIONS  << " iterations\n\n";

    // --- Start acceptor thread ---
    TcpAcceptor raw_acceptor;
    if (!raw_acceptor.listen(0).has_value()) {
        std::cerr << "Failed to listen\n";
        return 1;
    }
    uint16_t port = raw_acceptor.local_port();

    std::atomic<bool> acceptor_running{true};
    std::atomic<bool> acceptor_ready{false};

    std::thread acceptor_thread([&] {
        auto client_result = raw_acceptor.accept();
        if (!client_result.has_value()) return;

        TcpSocket client{*client_result};
        SessionConfig sc;
        sc.sender_comp_id = "ACCEPTOR";
        sc.target_comp_id = "BENCH";
        sc.heart_bt_int = 60;
        sc.validate_comp_ids = false;
        SessionManager session{sc};

        SessionCallbacks cbs;
        cbs.on_send = [&client](std::span<const char> data) -> bool {
            auto r = client.send(data);
            return r.has_value() && *r > 0;
        };
        cbs.on_logon = [&acceptor_ready]() {
            acceptor_ready.store(true, std::memory_order_release);
        };
        cbs.on_app_message = [&session](const ParsedMessage& msg) {
            if (msg.msg_type() == msg_type::NewOrderSingle) {
                auto er = fix44::ExecutionReport::Builder{}
                    .order_id("B001")
                    .exec_id("E001")
                    .exec_type(ExecType::New)
                    .ord_status(OrdStatus::New)
                    .symbol(msg.get_string(tag::Symbol::value))
                    .side(Side::Buy)
                    .leaves_qty(Qty::from_int(100))
                    .cum_qty(Qty::from_int(0))
                    .avg_px(FixedPrice::from_double(0.0))
                    .cl_ord_id(msg.get_string(tag::ClOrdID::value))
                    .transact_time("20260601-12:00:00.000");
                (void)session.send_app_message(er);
            }
        };
        session.set_callbacks(std::move(cbs));
        session.on_connect();

        SocketBridge<> bridge{client, session};
        while (acceptor_running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(1);
        }
    });

    // --- Initiator setup ---
    SessionConfig config;
    config.sender_comp_id = "BENCH";
    config.target_comp_id = "ACCEPTOR";
    config.heart_bt_int = 60;
    config.validate_comp_ids = false;

    SessionManager session{config};
    TcpSocket sock;
    if (!sock.connect("127.0.0.1", port).has_value()) {
        std::cerr << "Failed to connect\n";
        return 1;
    }

    std::atomic<uint64_t> recv_timestamp{0};

    SessionCallbacks cbs;
    cbs.on_send = [&sock](std::span<const char> data) -> bool {
        auto r = sock.send(data);
        return r.has_value() && *r > 0;
    };
    cbs.on_app_message = [&recv_timestamp]([[maybe_unused]] const ParsedMessage& msg) {
        recv_timestamp.store(
            static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()),
            std::memory_order_release);
    };
    session.set_callbacks(std::move(cbs));
    session.on_connect();
    (void)session.initiate_logon();

    SocketBridge<> bridge{sock, session};

    // Wait for logon
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.state() != SessionState::Active &&
           std::chrono::steady_clock::now() < deadline) {
        bridge.poll_once(1);
    }
    if (session.state() != SessionState::Active) {
        std::cerr << "Logon failed\n";
        return 1;
    }
    std::cout << "Session active. Running benchmark...\n";

    // --- Warmup ---
    for (int i = 0; i < WARMUP_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "W%06d", i);

        recv_timestamp.store(0, std::memory_order_release);

        auto nos = fix44::NewOrderSingle::Builder{}
            .cl_ord_id(id)
            .symbol("AAPL")
            .side(Side::Buy)
            .transact_time("20260601-12:00:00.000")
            .order_qty(Qty::from_int(100))
            .ord_type(OrdType::Limit)
            .price(FixedPrice::from_double(150.25));
        (void)session.send_app_message(nos);

        while (recv_timestamp.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }
    }
    std::cout << "Warmup complete.\n";

    // --- Benchmark ---
    std::vector<double> latencies;
    latencies.reserve(BENCH_ITERATIONS);

    for (int i = 0; i < BENCH_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "B%06d", i);

        recv_timestamp.store(0, std::memory_order_release);

        auto t0 = std::chrono::steady_clock::now();

        auto nos = fix44::NewOrderSingle::Builder{}
            .cl_ord_id(id)
            .symbol("AAPL")
            .side(Side::Buy)
            .transact_time("20260601-12:00:00.000")
            .order_qty(Qty::from_int(100))
            .ord_type(OrdType::Limit)
            .price(FixedPrice::from_double(150.25));
        (void)session.send_app_message(nos);

        while (recv_timestamp.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }

        auto t1 = std::chrono::steady_clock::now();
        double latency_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        latencies.push_back(latency_us);
    }

    auto stats = compute_stats(latencies);
    print_stats("NOS -> ER Round-Trip (loopback TCP)", stats);

    // Throughput estimate
    double total_time_s = 0;
    for (auto v : latencies) total_time_s += v;
    total_time_s /= 1e6;
    double msg_per_sec = static_cast<double>(BENCH_ITERATIONS) / total_time_s;
    std::cout << "\n  Throughput: " << std::fixed << std::setprecision(0)
              << msg_per_sec << " round-trips/sec\n";

    // Cleanup
    acceptor_running.store(false, std::memory_order_release);
    raw_acceptor.close();
    sock.close();
    acceptor_thread.join();

    return 0;
}
