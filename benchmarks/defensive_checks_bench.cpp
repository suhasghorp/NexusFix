// defensive_checks_bench.cpp
// TICKET_486: Measure overhead of QuickFIX-parity defensive checks
// (CompID validation, SendingTime accuracy, PossDup+OrigSendingTime)

#include <chrono>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>

#include "nexusfix/nexusfix.hpp"
#include "benchmark_utils.hpp"

using namespace nfx;

namespace {

struct NullSend {
    bool operator()(std::span<const char>) { return true; }
};

std::string build_hb_current_time(std::string_view sender,
                                  std::string_view target,
                                  uint32_t seq) {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d:%02d:%02d.000",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    MessageAssembler asm_;
    auto msg = fix44::Heartbeat::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq)
        .sending_time(std::string_view{ts_buf})
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

std::string build_logon(std::string_view sender, std::string_view target,
                        uint32_t seq) {
    auto now = std::chrono::system_clock::now();
    auto now_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&now_t, &tm);
    char ts_buf[32];
    std::snprintf(ts_buf, sizeof(ts_buf), "%04d%02d%02d-%02d:%02d:%02d.000",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);

    MessageAssembler asm_;
    auto msg = fix44::Logon::Builder{}
        .sender_comp_id(sender)
        .target_comp_id(target)
        .msg_seq_num(seq)
        .sending_time(std::string_view{ts_buf})
        .encrypt_method(0)
        .heart_bt_int(30)
        .build(asm_);
    return std::string(msg.data(), msg.size());
}

struct BenchResult {
    nfx::bench::LatencyStats stats;
    const char* label;
};

BenchResult run_session_bench(const char* label, bool validate_comp_ids,
                              bool check_latency, size_t iterations) {
    SessionConfig config{};
    config.sender_comp_id = "SENDER";
    config.target_comp_id = "TARGET";
    config.heart_bt_int = 30;
    config.validate_comp_ids = validate_comp_ids;
    config.check_latency = check_latency;
    config.max_latency = 120;

    SessionManager session(config);
    NullSend null_send;
    SessionCallbacks cbs;
    cbs.on_send = [&](std::span<const char> d) -> bool { return null_send(d); };
    session.set_callbacks(std::move(cbs));

    session.on_connect();
    (void)session.initiate_logon();
    auto logon_resp = build_logon("TARGET", "SENDER", 1);
    session.on_data_received(
        std::span<const char>{logon_resp.data(), logon_resp.size()});

    // Pre-build all messages (warmup + measured) to avoid syscall noise
    constexpr size_t WARMUP = 1000;
    std::vector<std::string> messages;
    messages.reserve(WARMUP + iterations);
    for (size_t i = 0; i < WARMUP + iterations; ++i) {
        messages.push_back(build_hb_current_time(
            "TARGET", "SENDER", static_cast<uint32_t>(i + 2)));
    }

    // Warmup
    for (size_t i = 0; i < WARMUP; ++i) {
        session.on_data_received(
            std::span<const char>{messages[i].data(), messages[i].size()});
    }

    double freq_ghz = nfx::bench::estimate_cpu_freq_ghz_busy();
    std::vector<uint64_t> cycles(iterations);

    for (size_t i = 0; i < iterations; ++i) {
        const auto& msg = messages[WARMUP + i];
        std::span<const char> data{msg.data(), msg.size()};

        uint64_t start = nfx::bench::rdtsc_vm_safe();
        session.on_data_received(data);
        uint64_t end = nfx::bench::rdtsc_vm_safe();
        cycles[i] = end - start;
    }

    BenchResult result;
    result.label = label;
    result.stats.compute(cycles, freq_ghz);
    return result;
}

void print_stats(const char* label, const nfx::bench::LatencyStats& s) {
    std::cout << "\n=== " << label << " ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Iterations: " << s.count << "\n";
    std::cout << "  Min:        " << std::setw(10) << s.min_ns << " ns\n";
    std::cout << "  Mean:       " << std::setw(10) << s.mean_ns << " ns\n";
    std::cout << "  P50:        " << std::setw(10) << s.p50_ns << " ns\n";
    std::cout << "  P90:        " << std::setw(10) << s.p90_ns << " ns\n";
    std::cout << "  P99:        " << std::setw(10) << s.p99_ns << " ns\n";
    std::cout << "  P99.9:      " << std::setw(10) << s.p999_ns << " ns\n";
    std::cout << "  Max:        " << std::setw(10) << s.max_ns << " ns\n";
    std::cout << "  StdDev:     " << std::setw(10) << s.stddev_ns << " ns\n";
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    size_t iterations = 100000;
    if (argc > 1) iterations = std::stoul(argv[1]);

    std::cout << "============================================================\n";
    std::cout << "  TICKET_486: Defensive Checks Overhead Benchmark\n";
    std::cout << "============================================================\n";
    std::cout << "Iterations: " << iterations << "\n";

    std::cout << "\nCalibrating CPU frequency...\n";
    double freq = nfx::bench::estimate_cpu_freq_ghz_busy();
    std::cout << "CPU frequency: " << std::fixed << std::setprecision(3)
              << freq << " GHz\n";

    // Run each config 3 times and take the best (lowest mean) to reduce noise
    constexpr int RUNS = 3;

    auto best_of = [&](const char* label, bool compid, bool latency) {
        BenchResult best{};
        best.stats.mean_ns = 1e18;
        best.label = label;
        for (int r = 0; r < RUNS; ++r) {
            auto result = run_session_bench(label, compid, latency, iterations);
            if (result.stats.mean_ns < best.stats.mean_ns) {
                best = result;
            }
        }
        return best;
    };

    auto baseline = best_of(
        "No defensive checks (validate_comp_ids=false, check_latency=false)",
        false, false);

    auto compid = best_of("CompID validation only", true, false);

    auto latency = best_of("SendingTime accuracy only", false, true);

    auto all = best_of("All defensive checks enabled", true, true);

    print_stats(baseline.label, baseline.stats);
    print_stats(compid.label, compid.stats);
    print_stats(latency.label, latency.stats);
    print_stats(all.label, all.stats);

    std::cout << "\n------------------------------------------------------------\n";
    std::cout << "                     OVERHEAD SUMMARY\n";
    std::cout << "------------------------------------------------------------\n\n";

    auto overhead = [](double with_ns, double without_ns) {
        return with_ns - without_ns;
    };
    auto pct = [](double with_ns, double without_ns) {
        return (with_ns - without_ns) / without_ns * 100.0;
    };

    std::cout << std::fixed << std::setprecision(2);
    std::cout << std::setw(35) << std::left << "Check"
              << std::setw(12) << "Mean (ns)"
              << std::setw(14) << "Overhead (ns)"
              << std::setw(12) << "Overhead (%)" << "\n";
    std::cout << std::string(73, '-') << "\n";

    std::cout << std::setw(35) << std::left << "Baseline (no checks)"
              << std::setw(12) << baseline.stats.mean_ns
              << std::setw(14) << "---"
              << std::setw(12) << "---" << "\n";

    std::cout << std::setw(35) << std::left << "CompID validation"
              << std::setw(12) << compid.stats.mean_ns
              << std::setw(14) << overhead(compid.stats.mean_ns, baseline.stats.mean_ns)
              << std::setw(12) << pct(compid.stats.mean_ns, baseline.stats.mean_ns) << "%\n";

    std::cout << std::setw(35) << std::left << "SendingTime accuracy"
              << std::setw(12) << latency.stats.mean_ns
              << std::setw(14) << overhead(latency.stats.mean_ns, baseline.stats.mean_ns)
              << std::setw(12) << pct(latency.stats.mean_ns, baseline.stats.mean_ns) << "%\n";

    std::cout << std::setw(35) << std::left << "All checks enabled"
              << std::setw(12) << all.stats.mean_ns
              << std::setw(14) << overhead(all.stats.mean_ns, baseline.stats.mean_ns)
              << std::setw(12) << pct(all.stats.mean_ns, baseline.stats.mean_ns) << "%\n";

    // Isolated check microbenchmarks
    std::cout << "\n------------------------------------------------------------\n";
    std::cout << "              ISOLATED CHECK MICROBENCHMARKS\n";
    std::cout << "------------------------------------------------------------\n";

    {
        // CompID comparison cost
        std::string_view sender = "TARGET";
        std::string_view target = "SENDER";
        std::string_view config_target = "TARGET";
        std::string_view config_sender = "SENDER";

        std::vector<uint64_t> cyc(iterations);
        for (size_t i = 0; i < iterations; ++i) {
            uint64_t s = nfx::bench::rdtsc_vm_safe();
            volatile bool match = (sender == config_target) &&
                                  (target == config_sender);
            (void)match;
            uint64_t e = nfx::bench::rdtsc_vm_safe();
            cyc[i] = e - s;
        }
        nfx::bench::LatencyStats st;
        st.compute(cyc, freq);
        std::cout << "\nCompID comparison (2x string_view ==): "
                  << std::fixed << std::setprecision(2)
                  << st.mean_ns << " ns mean, "
                  << st.p50_ns << " ns P50\n";
    }

    {
        // SendingTime parse + system_clock::now() cost
        std::string_view ts = "20260619-12:00:00.000";

        auto parse_check = [](std::string_view sending_time) -> bool {
            if (sending_time.size() < 17) return false;
            auto parse2 = [](const char* p) -> int {
                return (p[0] - '0') * 10 + (p[1] - '0');
            };
            auto parse4 = [](const char* p) -> int {
                return (p[0] - '0') * 1000 + (p[1] - '0') * 100 +
                       (p[2] - '0') * 10 + (p[3] - '0');
            };
            const char* s = sending_time.data();
            int year = parse4(s);
            int month = parse2(s + 4);
            int day = parse2(s + 6);
            int hour = parse2(s + 9);
            int minute = parse2(s + 12);
            int second = parse2(s + 15);
            auto days_from_ymd = [](int y, int m, int d) -> int64_t {
                y -= (m <= 2);
                int era = y / 400;
                int yoe = y - era * 400;
                int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
                int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
                return era * 146097 + doe - 719468;
            };
            int64_t msg_epoch = days_from_ymd(year, month, day) * 86400 +
                                hour * 3600 + minute * 60 + second;
            auto now = std::chrono::system_clock::now();
            auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch()).count();
            int64_t diff = now_epoch - msg_epoch;
            if (diff < 0) diff = -diff;
            return diff <= 120;
        };

        std::vector<uint64_t> cyc(iterations);
        for (size_t i = 0; i < iterations; ++i) {
            uint64_t s = nfx::bench::rdtsc_vm_safe();
            volatile bool ok = parse_check(ts);
            (void)ok;
            uint64_t e = nfx::bench::rdtsc_vm_safe();
            cyc[i] = e - s;
        }
        nfx::bench::LatencyStats st;
        st.compute(cyc, freq);
        std::cout << "SendingTime parse + now(): "
                  << std::fixed << std::setprecision(2)
                  << st.mean_ns << " ns mean, "
                  << st.p50_ns << " ns P50\n";
    }

    {
        // system_clock::now() alone
        std::vector<uint64_t> cyc(iterations);
        for (size_t i = 0; i < iterations; ++i) {
            uint64_t s = nfx::bench::rdtsc_vm_safe();
            volatile auto now = std::chrono::system_clock::now();
            (void)now;
            uint64_t e = nfx::bench::rdtsc_vm_safe();
            cyc[i] = e - s;
        }
        nfx::bench::LatencyStats st;
        st.compute(cyc, freq);
        std::cout << "system_clock::now() alone: "
                  << std::fixed << std::setprecision(2)
                  << st.mean_ns << " ns mean, "
                  << st.p50_ns << " ns P50\n";
    }

    std::cout << "\n============================================================\n";
    std::cout << "  Verdict: Overhead should be < 5% for production use\n";
    std::cout << "  Note: SendingTime check cost dominated by system_clock::now()\n";
    std::cout << "  Disable with check_latency=false for lowest latency\n";
    std::cout << "============================================================\n";

    return 0;
}
