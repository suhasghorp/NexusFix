/// @file test_memory_message_store.cpp
/// @brief Tests for MemoryMessageStore (TICKET_479 Phase 1A)

#include <catch2/catch_test_macros.hpp>

#include "nexusfix/store/memory_message_store.hpp"

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

using namespace nfx::store;

namespace {

/// Helper: create a message payload from a string
std::vector<char> make_msg(std::string_view sv) {
    return {sv.begin(), sv.end()};
}

} // namespace

// ============================================================================
// Construction and Configuration
// ============================================================================

TEST_CASE("MemoryMessageStore construction", "[store][regression]") {
    SECTION("Construct with session_id string") {
        MemoryMessageStore store("TEST_SESSION");
        REQUIRE(store.session_id() == "TEST_SESSION");
        REQUIRE(store.message_count() == 0);
        REQUIRE(store.bytes_used() == 0);
        REQUIRE(store.get_next_sender_seq_num() == 1);
        REQUIRE(store.get_next_target_seq_num() == 1);
    }

    SECTION("Construct with Config") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "CFG_SESSION";
        cfg.max_messages = 500;
        cfg.max_bytes = 1'000'000;
        MemoryMessageStore store(cfg);
        REQUIRE(store.session_id() == "CFG_SESSION");
    }
}

// ============================================================================
// Store and Retrieve
// ============================================================================

TEST_CASE("MemoryMessageStore store and retrieve", "[store][regression]") {
    MemoryMessageStore store("STORE_TEST");

    const std::string payload = "8=FIX.4.4\x01" "9=5\x01" "35=D\x01" "10=000\x01";
    auto msg = make_msg(payload);

    SECTION("Store single message and retrieve by seq_num") {
        REQUIRE(store.store(1, msg));
        REQUIRE(store.message_count() == 1);
        REQUIRE(store.contains(1));

        auto retrieved = store.retrieve(1);
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->size() == msg.size());
        REQUIRE(std::equal(retrieved->begin(), retrieved->end(), msg.begin()));
    }

    SECTION("Retrieve non-existent seq_num returns nullopt") {
        auto result = store.retrieve(999);
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("Store multiple messages and retrieve_range") {
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));
        REQUIRE(store.store(3, msg));

        auto range = store.retrieve_range(1, 3);
        REQUIRE(range.size() == 3);
    }

    SECTION("Retrieve range with gaps in sequence") {
        REQUIRE(store.store(1, msg));
        // seq 2 missing
        REQUIRE(store.store(3, msg));
        REQUIRE(store.store(5, msg));

        auto range = store.retrieve_range(1, 5);
        REQUIRE(range.size() == 3);  // Only 1, 3, 5 present
    }

    SECTION("Retrieve range with end_seq 0 means to max") {
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));
        REQUIRE(store.store(3, msg));

        auto range = store.retrieve_range(1, 0);
        REQUIRE(range.size() == 3);
    }

    SECTION("Store preserves exact bytes") {
        const std::string binary_payload = "BINARY\x00\x01\x02\xFF";
        auto bin_msg = std::vector<char>(binary_payload.begin(), binary_payload.end());
        REQUIRE(store.store(1, bin_msg));

        auto retrieved = store.retrieve(1);
        REQUIRE(retrieved.has_value());
        REQUIRE(*retrieved == bin_msg);
    }

    SECTION("Duplicate seq_num is rejected") {
        REQUIRE(store.store(1, msg));
        REQUIRE_FALSE(store.store(1, msg));
        REQUIRE(store.message_count() == 1);
    }
}

// ============================================================================
// Sequence Number Persistence
// ============================================================================

TEST_CASE("MemoryMessageStore sequence numbers", "[store][regression]") {
    MemoryMessageStore store("SEQ_TEST");

    SECTION("set/get next_sender_seq_num") {
        store.set_next_sender_seq_num(42);
        REQUIRE(store.get_next_sender_seq_num() == 42);
    }

    SECTION("set/get next_target_seq_num") {
        store.set_next_target_seq_num(100);
        REQUIRE(store.get_next_target_seq_num() == 100);
    }
}

// ============================================================================
// Reset
// ============================================================================

TEST_CASE("MemoryMessageStore reset", "[store][regression]") {
    MemoryMessageStore store("RESET_TEST");

    auto msg = make_msg("test_message");
    REQUIRE(store.store(1, msg));
    REQUIRE(store.store(2, msg));
    store.set_next_sender_seq_num(10);
    store.set_next_target_seq_num(20);

    store.reset();

    REQUIRE(store.message_count() == 0);
    REQUIRE(store.bytes_used() == 0);
    REQUIRE(store.get_next_sender_seq_num() == 1);
    REQUIRE(store.get_next_target_seq_num() == 1);
    REQUIRE_FALSE(store.contains(1));
    REQUIRE_FALSE(store.contains(2));
}

// ============================================================================
// Stats and Metrics
// ============================================================================

TEST_CASE("MemoryMessageStore stats tracking", "[store][regression]") {
    MemoryMessageStore store("STATS_TEST");

    auto msg = make_msg("stats_payload");

    SECTION("message_count and bytes_used track correctly") {
        REQUIRE(store.store(1, msg));
        REQUIRE(store.message_count() == 1);
        REQUIRE(store.bytes_used() == msg.size());

        REQUIRE(store.store(2, msg));
        REQUIRE(store.message_count() == 2);
        REQUIRE(store.bytes_used() == msg.size() * 2);
    }

    SECTION("stats reports stored and retrieved counts") {
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));
        [[maybe_unused]] auto _ = store.retrieve(1);

        auto s = store.stats();
        REQUIRE(s.messages_stored == 2);
        REQUIRE(s.messages_retrieved == 1);
        REQUIRE(s.bytes_stored == msg.size() * 2);
        REQUIRE(s.store_failures == 0);
    }

    SECTION("pool_metrics reports capacity and usage") {
        auto pm = store.pool_metrics();
        REQUIRE(pm.pool_capacity > 0);

        REQUIRE(store.store(1, msg));
        pm = store.pool_metrics();
        REQUIRE(pm.bytes_allocated > 0);
    }
}

// ============================================================================
// Capacity and Eviction
// ============================================================================

TEST_CASE("MemoryMessageStore capacity boundary", "[store][regression]") {
    SECTION("Evicts oldest when full with evict_oldest=true") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "EVICT_TEST";
        cfg.max_messages = 3;
        cfg.evict_oldest = true;
        MemoryMessageStore store(cfg);

        auto msg = make_msg("data");
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));
        REQUIRE(store.store(3, msg));
        REQUIRE(store.message_count() == 3);

        // Storing 4th should evict seq 1
        REQUIRE(store.store(4, msg));
        REQUIRE(store.message_count() == 3);
        REQUIRE_FALSE(store.contains(1));
        REQUIRE(store.contains(4));
    }

    SECTION("Rejects when full with evict_oldest=false") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "REJECT_TEST";
        cfg.max_messages = 2;
        cfg.evict_oldest = false;
        MemoryMessageStore store(cfg);

        auto msg = make_msg("data");
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));
        REQUIRE_FALSE(store.store(3, msg));
        REQUIRE(store.message_count() == 2);

        auto s = store.stats();
        REQUIRE(s.store_failures == 1);
    }

    // TICKET_497 Phase 1: byte-limit branch of the capacity check
    // (message-count limit is exercised above; the OR'd byte-limit is not).
    SECTION("Evicts on byte-limit with evict_oldest=true") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "BYTE_EVICT";
        cfg.max_messages = 1000;   // count is not the binding limit
        cfg.max_bytes = 12;        // binding limit: two 5-byte msgs fit, third evicts
        cfg.evict_oldest = true;
        MemoryMessageStore store(cfg);

        auto msg = make_msg("55555");  // 5 bytes each
        REQUIRE(store.store(1, msg));
        REQUIRE(store.store(2, msg));  // 10 bytes, still under 12
        REQUIRE(store.message_count() == 2);

        // 3rd would push to 15 > 12: evicts oldest (seq 1), stays at 2 messages
        REQUIRE(store.store(3, msg));
        REQUIRE(store.message_count() == 2);
        REQUIRE_FALSE(store.contains(1));
        REQUIRE(store.contains(3));
        REQUIRE(store.bytes_used() == 10);
    }

    SECTION("Rejects on byte-limit with evict_oldest=false") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "BYTE_REJECT";
        cfg.max_messages = 1000;
        cfg.max_bytes = 8;
        cfg.evict_oldest = false;
        MemoryMessageStore store(cfg);

        auto msg = make_msg("55555");  // 5 bytes
        REQUIRE(store.store(1, msg));
        // 2nd would push to 10 > 8: rejected, message and bytes unchanged
        REQUIRE_FALSE(store.store(2, msg));
        REQUIRE(store.message_count() == 1);
        REQUIRE(store.bytes_used() == 5);
        REQUIRE(store.stats().store_failures == 1);
    }

    // Exercises evict_oldest_locked()'s "find new minimum" scan (the loop
    // over remaining messages), reached only when >1 message survives eviction.
    SECTION("Eviction recomputes min_seq over remaining messages") {
        MemoryMessageStore::Config cfg;
        cfg.session_id = "MIN_RECOMPUTE";
        cfg.max_messages = 3;
        cfg.evict_oldest = true;
        MemoryMessageStore store(cfg);

        auto msg = make_msg("x");
        // Store out of order so min_seq is not the first inserted.
        REQUIRE(store.store(10, msg));
        REQUIRE(store.store(5, msg));   // min_seq = 5
        REQUIRE(store.store(20, msg));
        REQUIRE(store.message_count() == 3);

        // 4th evicts min (seq 5); loop must recompute min_seq = 10 over {10,20}.
        REQUIRE(store.store(30, msg));
        REQUIRE(store.message_count() == 3);
        REQUIRE_FALSE(store.contains(5));
        REQUIRE(store.contains(10));

        // 5th evicts new min (seq 10); recompute again over {20,30}.
        REQUIRE(store.store(40, msg));
        REQUIRE_FALSE(store.contains(10));
        REQUIRE(store.contains(20));
        REQUIRE(store.contains(30));
        REQUIRE(store.contains(40));
    }
}

// ============================================================================
// flush (no-op contract)
// ============================================================================

TEST_CASE("MemoryMessageStore flush is no-op", "[store][regression]") {
    MemoryMessageStore store("FLUSH_TEST");
    auto msg = make_msg("data");
    REQUIRE(store.store(1, msg));
    store.flush();  // Should not crash or change state
    REQUIRE(store.message_count() == 1);
    REQUIRE(store.contains(1));
}

// ============================================================================
// Thread Safety Smoke Test
// ============================================================================

TEST_CASE("MemoryMessageStore concurrent access", "[store][regression]") {
    MemoryMessageStore::Config cfg;
    cfg.session_id = "THREAD_TEST";
    cfg.max_messages = 10000;
    MemoryMessageStore store(cfg);

    constexpr int per_thread = 500;
    constexpr int num_threads = 4;

    auto writer = [&](int offset) {
        for (int i = 0; i < per_thread; ++i) {
            auto seq = static_cast<uint32_t>(offset * per_thread + i + 1);
            auto payload = std::to_string(seq);
            [[maybe_unused]] bool ok = store.store(seq, std::span<const char>(payload.data(), payload.size()));
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back(writer, t);
    }
    for (auto& t : threads) {
        t.join();
    }

    // All unique seq_nums, so total should be num_threads * per_thread
    REQUIRE(store.message_count() == num_threads * per_thread);
}
