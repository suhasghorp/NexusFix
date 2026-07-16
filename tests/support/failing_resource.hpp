/// @file failing_resource.hpp
/// @brief Fault-injecting std::pmr::memory_resource for OOM testing (TICKET_497 Phase 2)
///
/// SQLite's OOM discipline: run a workload repeatedly, failing the Nth allocation
/// for N = 1, 2, 3, ... until the workload completes with no injected failure.
/// Every injected failure must leave the object in a valid state (no crash, no
/// leak, error surfaced through its documented channel).
///
/// FailingResource wraps an upstream memory_resource and throws std::bad_alloc on
/// a chosen allocation. It is a test-only header; it lives under tests/support/
/// and is never compiled into the library.

#pragma once

#include <cstddef>
#include <limits>
#include <memory_resource>

namespace nfx::test {

/// A std::pmr::memory_resource that forwards to an upstream resource but fails
/// (throws std::bad_alloc) on the Nth allocation request.
///
/// Counting is 1-based: fail_after(1) fails the very first allocate() call,
/// fail_after(0) or the default never injects a failure (pass-through).
class FailingResource : public std::pmr::memory_resource {
public:
    explicit FailingResource(
        std::pmr::memory_resource* upstream = std::pmr::new_delete_resource()) noexcept
        : upstream_(upstream) {}

    /// Inject a std::bad_alloc on the Nth allocation (1-based). N == 0 disables.
    void fail_after(std::size_t n) noexcept {
        fail_at_ = n;
        alloc_count_ = 0;
    }

    /// Disable injection (pass-through mode).
    void disable() noexcept { fail_at_ = 0; }

    /// Number of allocation requests seen so far (including the failed one).
    [[nodiscard]] std::size_t allocation_count() const noexcept { return alloc_count_; }

    /// Number of allocations that were actually satisfied (not counting the
    /// injected failure). Used to detect leaks: after teardown this must be 0.
    [[nodiscard]] std::size_t live_allocations() const noexcept { return live_; }

    /// True if the injected failure point was reached during the run.
    [[nodiscard]] bool triggered() const noexcept { return triggered_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++alloc_count_;
        if (fail_at_ != 0 && alloc_count_ == fail_at_) {
            triggered_ = true;
            throw std::bad_alloc{};
        }
        void* p = upstream_->allocate(bytes, alignment);
        ++live_;
        return p;
    }

    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        if (live_ > 0) --live_;
        upstream_->deallocate(p, bytes, alignment);
    }

    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource* upstream_;
    std::size_t fail_at_{0};       // 1-based allocation index to fail; 0 = disabled
    std::size_t alloc_count_{0};   // allocations seen
    std::size_t live_{0};          // outstanding allocations (leak detector)
    bool triggered_{false};
};

}  // namespace nfx::test
