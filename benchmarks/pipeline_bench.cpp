// pipeline_bench.cpp (TICKET_483)
// NexusFIX End-to-End Pipeline Benchmark
//
// Modes:
//   --roundtrip   NOS->ER round-trip latency (RDTSC + chrono)
//   --halftip     Send-side half-trip: build + serialize + TCP send
//   --throughput  Sustained throughput saturation (burst N, measure drain)
//   --api         FixInitiator/FixAcceptor public API overhead
//   (default)     All modes

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>
#include <thread>
#include <vector>

#include "nexusfix/engine/fix_initiator.hpp"
#include "nexusfix/engine/fix_acceptor.hpp"
#include "nexusfix/engine/socket_bridge.hpp"
#include "nexusfix/session/session_manager.hpp"
#include "nexusfix/transport/tcp_transport.hpp"
#include "nexusfix/messages/fix44/execution_report.hpp"
#include "nexusfix/messages/fix44/new_order_single.hpp"
#include "nexusfix/platform/platform.hpp"
#include "benchmark_utils.hpp"

using namespace nfx;

// ============================================================================
// Configuration
// ============================================================================

static constexpr int WARMUP_ITERATIONS = 2000;
static constexpr int BENCH_ITERATIONS  = 10000;
static constexpr int THROUGHPUT_BURST_SIZES[] = {100, 1000, 5000};

// ============================================================================
// Statistics (microseconds)
// ============================================================================

struct PipelineStats {
    double min_us{}, max_us{}, mean_us{}, stddev_us{};
    double p50_us{}, p90_us{}, p99_us{}, p999_us{};
    size_t count{};
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
    std::cout << "  Min:       " << std::setw(10) << s.min_us  << " us\n";
    std::cout << "  P50:       " << std::setw(10) << s.p50_us  << " us\n";
    std::cout << "  P90:       " << std::setw(10) << s.p90_us  << " us\n";
    std::cout << "  P99:       " << std::setw(10) << s.p99_us  << " us\n";
    std::cout << "  P99.9:     " << std::setw(10) << s.p999_us << " us\n";
    std::cout << "  Max:       " << std::setw(10) << s.max_us  << " us\n";
    std::cout << "  Mean:      " << std::setw(10) << s.mean_us << " us\n";
    std::cout << "  Stddev:    " << std::setw(10) << s.stddev_us << " us\n";

    if (s.p50_us > 0) {
        std::cout << "  P99/P50:   " << std::setw(10) << std::setprecision(2)
                  << (s.p99_us / s.p50_us) << "x (jitter ratio)\n";
    }

    double total_time_s = s.mean_us * static_cast<double>(s.count) / 1e6;
    if (total_time_s > 0) {
        double msg_per_sec = static_cast<double>(s.count) / total_time_s;
        std::cout << "  Throughput: " << std::setprecision(0)
                  << msg_per_sec << " round-trips/sec\n";
    }
}

// ============================================================================
// Acceptor thread helper (raw SessionManager wiring)
// ============================================================================

struct AcceptorContext {
    TcpAcceptor listener;
    std::atomic<bool> running{true};
    std::atomic<bool> logon_done{false};
    std::thread thread;

    uint16_t start() {
        if (!listener.listen(0).has_value()) {
            std::cerr << "Acceptor: listen failed\n";
            return 0;
        }
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
        auto client_result = listener.accept();
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
        cbs.on_logon = [this]() {
            logon_done.store(true, std::memory_order_release);
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
        while (running.load(std::memory_order_acquire) && client.is_connected()) {
            bridge.poll_once(1);
        }
    }
};

// ============================================================================
// NOS builder helper
// ============================================================================

static auto make_nos(const char* id) {
    return fix44::NewOrderSingle::Builder{}
        .cl_ord_id(id)
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260601-12:00:00.000")
        .order_qty(Qty::from_int(100))
        .ord_type(OrdType::Limit)
        .price(FixedPrice::from_double(150.25));
}

// ============================================================================
// Establish session (connect + logon + wait for Active)
// ============================================================================

static bool establish_session(TcpSocket& sock, SessionManager& session,
                              SocketBridge<>& bridge, AcceptorContext& acceptor,
                              uint16_t port) {
    if (!sock.connect("127.0.0.1", port).has_value()) {
        std::cerr << "Connect failed\n";
        return false;
    }

    session.on_connect();
    (void)session.initiate_logon();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (session.state() != SessionState::Active &&
           std::chrono::steady_clock::now() < deadline) {
        bridge.poll_once(1);
    }

    if (session.state() != SessionState::Active) {
        std::cerr << "Logon timed out\n";
        return false;
    }

    return true;
}

// ============================================================================
// Mode 1: Round-trip latency (chrono + RDTSC)
// ============================================================================

static int run_roundtrip() {
    std::cout << "\n============================================================\n";
    std::cout << "Mode: Round-trip Latency (NOS -> ER over TCP loopback)\n";
    std::cout << "============================================================\n";

    AcceptorContext acceptor;
    uint16_t port = acceptor.start();
    if (port == 0) return 1;

    SessionConfig config;
    config.sender_comp_id = "BENCH";
    config.target_comp_id = "ACCEPTOR";
    config.heart_bt_int = 60;
    config.validate_comp_ids = false;

    SessionManager session{config};
    TcpSocket sock;

    std::atomic<uint64_t> recv_flag{0};

    SessionCallbacks cbs;
    cbs.on_send = [&sock](std::span<const char> data) -> bool {
        auto r = sock.send(data);
        return r.has_value() && *r > 0;
    };
    cbs.on_app_message = [&recv_flag]([[maybe_unused]] const ParsedMessage& msg) {
        recv_flag.store(1, std::memory_order_release);
    };
    session.set_callbacks(std::move(cbs));

    SocketBridge<> bridge{sock, session};
    if (!establish_session(sock, session, bridge, acceptor, port)) return 1;

    // Warmup
    std::cout << "Warming up (" << WARMUP_ITERATIONS << " iterations)...\n";
    for (int i = 0; i < WARMUP_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "W%06d", i);
        recv_flag.store(0, std::memory_order_release);
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        while (recv_flag.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }
    }

    // --- Chrono measurement ---
    std::cout << "Running chrono benchmark (" << BENCH_ITERATIONS << " iterations)...\n";
    std::vector<double> chrono_latencies;
    chrono_latencies.reserve(BENCH_ITERATIONS);

    for (int i = 0; i < BENCH_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "C%06d", i);
        recv_flag.store(0, std::memory_order_release);

        auto t0 = std::chrono::steady_clock::now();
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        while (recv_flag.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }
        auto t1 = std::chrono::steady_clock::now();

        chrono_latencies.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    auto chrono_stats = compute_stats(chrono_latencies);
    print_stats("NOS -> ER Round-Trip (chrono, loopback TCP)", chrono_stats);

    // --- RDTSC measurement ---
#if NFX_ARCH_X64
    std::cout << "\nRunning RDTSC benchmark (" << BENCH_ITERATIONS << " iterations)...\n";
    double freq_ghz = bench::estimate_cpu_freq_ghz_busy();
    std::cout << "  CPU frequency: " << std::fixed << std::setprecision(3)
              << freq_ghz << " GHz\n";

    std::vector<double> rdtsc_latencies;
    rdtsc_latencies.reserve(BENCH_ITERATIONS);

    for (int i = 0; i < BENCH_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "R%06d", i);
        recv_flag.store(0, std::memory_order_release);

        uint64_t t0 = bench::rdtsc_vm_safe();
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        while (recv_flag.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }
        uint64_t t1 = bench::rdtsc_vm_safe();

        double us = bench::cycles_to_ns(t1 - t0, freq_ghz) / 1000.0;
        rdtsc_latencies.push_back(us);
    }

    auto rdtsc_stats = compute_stats(rdtsc_latencies);
    print_stats("NOS -> ER Round-Trip (RDTSC, loopback TCP)", rdtsc_stats);
#else
    std::cout << "\n  [RDTSC skipped: not x86_64]\n";
#endif

    acceptor.stop();
    return 0;
}

// ============================================================================
// Mode 2: Half-trip (send-side only)
// ============================================================================

static int run_halftrip() {
    std::cout << "\n============================================================\n";
    std::cout << "Mode: Half-trip Send-side (build + serialize + TCP send)\n";
    std::cout << "============================================================\n";

    AcceptorContext acceptor;
    uint16_t port = acceptor.start();
    if (port == 0) return 1;

    SessionConfig config;
    config.sender_comp_id = "BENCH";
    config.target_comp_id = "ACCEPTOR";
    config.heart_bt_int = 60;
    config.validate_comp_ids = false;

    SessionManager session{config};
    TcpSocket sock;

    std::atomic<uint64_t> send_done_ts{0};

    SessionCallbacks cbs;
    cbs.on_send = [&sock, &send_done_ts](std::span<const char> data) -> bool {
        auto r = sock.send(data);
#if NFX_ARCH_X64
        send_done_ts.store(bench::rdtsc_vm_safe(), std::memory_order_release);
#else
        send_done_ts.store(
            static_cast<uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()),
            std::memory_order_release);
#endif
        return r.has_value() && *r > 0;
    };
    std::atomic<uint64_t> recv_flag{0};
    cbs.on_app_message = [&recv_flag]([[maybe_unused]] const ParsedMessage& msg) {
        recv_flag.store(1, std::memory_order_release);
    };
    session.set_callbacks(std::move(cbs));

    SocketBridge<> bridge{sock, session};
    if (!establish_session(sock, session, bridge, acceptor, port)) return 1;

    // Warmup
    for (int i = 0; i < WARMUP_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "HW%05d", i);
        recv_flag.store(0, std::memory_order_release);
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        while (recv_flag.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }
    }

#if NFX_ARCH_X64
    double freq_ghz = bench::estimate_cpu_freq_ghz_busy();

    std::vector<double> halfs;
    halfs.reserve(BENCH_ITERATIONS);

    std::cout << "Running RDTSC half-trip (" << BENCH_ITERATIONS << " iterations)...\n";

    for (int i = 0; i < BENCH_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "H%06d", i);
        recv_flag.store(0, std::memory_order_release);
        send_done_ts.store(0, std::memory_order_release);

        uint64_t t0 = bench::rdtsc_vm_safe();
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        uint64_t t1 = send_done_ts.load(std::memory_order_acquire);

        // Drain the ER so the session stays in sync
        while (recv_flag.load(std::memory_order_acquire) == 0) {
            bridge.poll_once(0);
        }

        if (t1 > t0) {
            halfs.push_back(bench::cycles_to_ns(t1 - t0, freq_ghz) / 1000.0);
        }
    }

    auto half_stats = compute_stats(halfs);
    print_stats("Send-side Half-trip (build+serialize+send)", half_stats);
#else
    std::cout << "  [Half-trip RDTSC skipped: not x86_64]\n";
#endif

    acceptor.stop();
    return 0;
}

// ============================================================================
// Mode 3: Sustained throughput (burst N, drain all)
// ============================================================================

static int run_throughput() {
    std::cout << "\n============================================================\n";
    std::cout << "Mode: Sustained Throughput (burst send, wait for all ERs)\n";
    std::cout << "============================================================\n";

    AcceptorContext acceptor;
    uint16_t port = acceptor.start();
    if (port == 0) return 1;

    SessionConfig config;
    config.sender_comp_id = "BENCH";
    config.target_comp_id = "ACCEPTOR";
    config.heart_bt_int = 60;
    config.validate_comp_ids = false;

    SessionManager session{config};
    TcpSocket sock;

    std::atomic<int> er_count{0};

    SessionCallbacks cbs;
    cbs.on_send = [&sock](std::span<const char> data) -> bool {
        auto r = sock.send(data);
        return r.has_value() && *r > 0;
    };
    cbs.on_app_message = [&er_count]([[maybe_unused]] const ParsedMessage& msg) {
        er_count.fetch_add(1, std::memory_order_release);
    };
    session.set_callbacks(std::move(cbs));

    SocketBridge<> bridge{sock, session};
    if (!establish_session(sock, session, bridge, acceptor, port)) return 1;

    // Warmup with ping-pong
    for (int i = 0; i < 500; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "TW%04d", i);
        int before = er_count.load(std::memory_order_acquire);
        auto nos = make_nos(id);
        (void)session.send_app_message(nos);
        while (er_count.load(std::memory_order_acquire) == before) {
            bridge.poll_once(0);
        }
    }

    for (int burst_size : THROUGHPUT_BURST_SIZES) {
        er_count.store(0, std::memory_order_release);

        auto t0 = std::chrono::steady_clock::now();

        int sent = 0;
        while (sent < burst_size) {
            char id[32];
            std::snprintf(id, sizeof(id), "T%06d", sent);
            auto nos = make_nos(id);
            (void)session.send_app_message(nos);
            ++sent;

            // Interleave polling every 50 sends to prevent TCP buffer saturation
            if (sent % 50 == 0) {
                bridge.poll_once(0);
            }
        }

        while (er_count.load(std::memory_order_acquire) < burst_size) {
            bridge.poll_once(0);
        }

        auto t1 = std::chrono::steady_clock::now();
        double elapsed_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        double elapsed_s  = elapsed_us / 1e6;
        double msg_per_sec = static_cast<double>(burst_size) / elapsed_s;
        double avg_us = elapsed_us / static_cast<double>(burst_size);

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "\n--- Burst " << burst_size << " NOS ---\n";
        std::cout << "  Total time:    " << std::setw(10) << elapsed_us << " us\n";
        std::cout << "  Avg/msg:       " << std::setw(10) << avg_us << " us\n";
        std::cout << "  Throughput:    " << std::setprecision(0) << std::setw(10)
                  << msg_per_sec << " round-trips/sec\n";
    }

    acceptor.stop();
    return 0;
}

// ============================================================================
// Mode 4: FixInitiator / FixAcceptor public API
// ============================================================================

static int run_api_bench() {
    std::cout << "\n============================================================\n";
    std::cout << "Mode: FixInitiator/FixAcceptor Public API Overhead\n";
    std::cout << "============================================================\n";

    // Acceptor setup
    AcceptorConfig aconfig;
    aconfig.port = 0;
    aconfig.sender_comp_id = "API_ACPT";
    aconfig.target_comp_id = "API_INIT";
    aconfig.heart_bt_int = 60;
    aconfig.validate_comp_ids = false;

    FixAcceptor acceptor{aconfig};

    SessionCallbacks acbs;
    // FixAcceptor runs session inside run_loop(), so we cannot call
    // session.send_app_message() from here. The ER echo must happen
    // in FixAcceptor's internal on_app_message. Since the current
    // FixAcceptor does not support that, we measure one-way: NOS send
    // via FixInitiator + acceptor receive, without ER echo.
    //
    // This measures the API overhead of FixInitiator::send() compared
    // to raw SessionManager::send_app_message().
    std::atomic<int> app_msg_count{0};
    acbs.on_app_message = [&app_msg_count]([[maybe_unused]] const ParsedMessage& msg) {
        app_msg_count.fetch_add(1, std::memory_order_release);
    };
    acceptor.set_callbacks(std::move(acbs));

    auto listen_result = acceptor.listen();
    if (!listen_result.has_value()) {
        std::cerr << "FixAcceptor: listen failed\n";
        return 1;
    }
    uint16_t port = *listen_result;
    acceptor.start_background();

    // Initiator setup
    InitiatorConfig iconfig;
    iconfig.host = "127.0.0.1";
    iconfig.port = port;
    iconfig.sender_comp_id = "API_INIT";
    iconfig.target_comp_id = "API_ACPT";
    iconfig.heart_bt_int = 60;
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
    if (!start_result.has_value()) {
        std::cerr << "FixInitiator: start failed\n";
        acceptor.stop();
        return 1;
    }

    bool active = initiator.poll_until([&] {
        return logon_ok.load(std::memory_order_acquire);
    }, 5000);
    if (!active) {
        std::cerr << "FixInitiator: logon timed out\n";
        acceptor.stop();
        return 1;
    }

    // Warmup
    for (int i = 0; i < 500; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "AW%04d", i);
        auto nos = make_nos(id);
        (void)initiator.send(nos);
        initiator.poll(1);
    }

    // Wait for warmup to drain
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (app_msg_count.load(std::memory_order_acquire) < 500 &&
           std::chrono::steady_clock::now() < deadline) {
        initiator.poll(1);
    }

    // Benchmark: measure send-side latency through FixInitiator API
    static constexpr int API_ITERATIONS = 5000;
    app_msg_count.store(0, std::memory_order_release);

    std::vector<double> api_send_latencies;
    api_send_latencies.reserve(API_ITERATIONS);

    std::cout << "Running API send benchmark (" << API_ITERATIONS << " iterations)...\n";

    for (int i = 0; i < API_ITERATIONS; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "A%06d", i);

        auto t0 = std::chrono::steady_clock::now();
        auto nos = make_nos(id);
        (void)initiator.send(nos);
        auto t1 = std::chrono::steady_clock::now();

        api_send_latencies.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());

        if (i % 100 == 0) {
            initiator.poll(0);
        }
    }

    // Drain remaining
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (app_msg_count.load(std::memory_order_acquire) < API_ITERATIONS &&
           std::chrono::steady_clock::now() < deadline) {
        initiator.poll(1);
    }

    auto api_stats = compute_stats(api_send_latencies);
    print_stats("FixInitiator::send() (one-way, build+serialize+TCP)", api_stats);

    int received = app_msg_count.load(std::memory_order_acquire);
    std::cout << "  Acceptor received: " << received << "/" << API_ITERATIONS
              << " messages\n";

    (void)initiator.stop();
    acceptor.stop();
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "NexusFIX End-to-End Pipeline Benchmark (TICKET_483)\n";
    std::cout << "====================================================\n";
    std::cout << "Platform: " << NFX_PLATFORM_NAME << " / " << NFX_ARCH_NAME << "\n";
    std::cout << "Compiler: " << NFX_COMPILER_NAME << "\n";
    std::cout << "Warmup:   " << WARMUP_ITERATIONS << "  Bench: " << BENCH_ITERATIONS << "\n";

#if NFX_ARCH_X64
    std::cout << "RDTSC:    available\n";
#else
    std::cout << "RDTSC:    not available (chrono fallback)\n";
#endif

    bool run_all = true;
    bool want_roundtrip  = false;
    bool want_halftrip   = false;
    bool want_throughput = false;
    bool want_api        = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--roundtrip") == 0) {
            want_roundtrip = true; run_all = false;
        } else if (std::strcmp(argv[i], "--halftrip") == 0) {
            want_halftrip = true; run_all = false;
        } else if (std::strcmp(argv[i], "--throughput") == 0) {
            want_throughput = true; run_all = false;
        } else if (std::strcmp(argv[i], "--api") == 0) {
            want_api = true; run_all = false;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            std::cout << "\nUsage: pipeline_bench [--roundtrip] [--halftrip] "
                         "[--throughput] [--api]\n";
            std::cout << "  Default: run all modes\n";
            return 0;
        }
    }

    int rc = 0;

    if (run_all || want_roundtrip) {
        rc |= run_roundtrip();
    }
    if (run_all || want_halftrip) {
        rc |= run_halftrip();
    }
    if (run_all || want_throughput) {
        rc |= run_throughput();
    }
    if (run_all || want_api) {
        rc |= run_api_bench();
    }

    std::cout << "\n====================================================\n";
    std::cout << "Pipeline benchmark complete.\n";

    return rc;
}
