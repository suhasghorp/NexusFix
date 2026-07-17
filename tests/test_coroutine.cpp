// SPDX-License-Identifier: MIT
// Copyright (c) 2025 StratCraftsAI
//
// TICKET_497_3 WS5: Branch coverage for session/coroutine.hpp
//
// Covers: Task<T>/Task<void> exception rethrow through get() and await_resume,
// FinalAwaiter continuation vs noop_coroutine, Task move-assign (self and with
// live handle), empty-task operator bool/done/resume, Generator begin-on-done /
// next() exhaustion / Iterator equality, SuspendIf both predicate outcomes,
// ReadyAwaitable.

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <optional>
#include <vector>

#include "nexusfix/session/coroutine.hpp"

using namespace nfx;

// ============================================================================
// Task<T> basic
// ============================================================================

static Task<int> make_value_task(int v) {
    co_return v;
}

static Task<int> make_throwing_task() {
    throw std::runtime_error("task error");
    co_return 0;
}

TEST_CASE("Task<int> get() returns value", "[coroutine][task][regression]") {
    auto t = make_value_task(42);
    REQUIRE(t.get() == 42);
}

TEST_CASE("Task<int> done() is false before resume, true after get()", "[coroutine][task][regression]") {
    auto t = make_value_task(7);
    REQUIRE_FALSE(t.done());  // initial_suspend = suspend_always
    REQUIRE(t.get() == 7);
    REQUIRE(t.done());
}

TEST_CASE("Task<int> get() rethrows exception", "[coroutine][task][regression]") {
    auto t = make_throwing_task();
    REQUIRE_THROWS_AS(t.get(), std::runtime_error);
}

TEST_CASE("Task<int> operator bool on valid and empty task", "[coroutine][task][regression]") {
    Task<int> empty;
    REQUIRE_FALSE(static_cast<bool>(empty));

    auto t = make_value_task(1);
    REQUIRE(static_cast<bool>(t));
}

TEST_CASE("Task<int> resume on empty task is safe", "[coroutine][task][regression]") {
    Task<int> empty;
    empty.resume();  // must not crash
    REQUIRE_FALSE(empty.done());
}

TEST_CASE("Task<int> done() on empty task is false", "[coroutine][task][regression]") {
    Task<int> empty;
    REQUIRE_FALSE(empty.done());
}

// ============================================================================
// Task<void> basic
// ============================================================================

static Task<void> make_void_task() {
    co_return;
}

static Task<void> make_void_throwing_task() {
    throw std::logic_error("void task error");
    co_return;
}

TEST_CASE("Task<void> get() completes without throw", "[coroutine][task][regression]") {
    auto t = make_void_task();
    t.get();
    REQUIRE(t.done());
}

TEST_CASE("Task<void> get() rethrows exception", "[coroutine][task][regression]") {
    auto t = make_void_throwing_task();
    REQUIRE_THROWS_AS(t.get(), std::logic_error);
}

TEST_CASE("Task<void> operator bool on empty task", "[coroutine][task][regression]") {
    Task<void> empty;
    REQUIRE_FALSE(static_cast<bool>(empty));
}

TEST_CASE("Task<void> resume on empty task is safe", "[coroutine][task][regression]") {
    Task<void> empty;
    empty.resume();
    REQUIRE_FALSE(empty.done());
}

// ============================================================================
// Task<T> move assignment
// ============================================================================

TEST_CASE("Task<int> move assign from live handle destroys old", "[coroutine][task][regression]") {
    auto t1 = make_value_task(10);
    auto t2 = make_value_task(20);
    t1 = std::move(t2);   // destroys t1's old handle, steals t2's handle
    REQUIRE(t1.get() == 20);
}

TEST_CASE("Task<int> move assign self is no-op", "[coroutine][task][regression]") {
    auto t = make_value_task(99);
    Task<int>& ref = t;
    t = std::move(ref);  // self-move: should survive without crash
    // After a self-move the coroutine frame may or may not be valid, but we
    // must not crash. Just check the object is still in a well-defined state.
    (void)t;
}

TEST_CASE("Task<void> move assign from live handle destroys old", "[coroutine][task][regression]") {
    auto t1 = make_void_task();
    auto t2 = make_void_task();
    t1 = std::move(t2);
    t1.get();
    REQUIRE(t1.done());
}

// ============================================================================
// Task<T> co_await (FinalAwaiter continuation vs noop_coroutine)
// ============================================================================

static Task<int> inner_task(int v) {
    co_return v * 2;
}

static Task<int> outer_task_with_continuation(int v) {
    int result = co_await inner_task(v);
    co_return result + 1;
}

TEST_CASE("Task<int> co_await drives FinalAwaiter continuation branch", "[coroutine][task][regression]") {
    auto t = outer_task_with_continuation(5);
    REQUIRE(t.get() == 11);  // 5*2 + 1
}

static Task<int> outer_await_ready(int v) {
    // inner_task will NOT be done immediately (initial_suspend = suspend_always)
    // so await_ready returns false, driving the await_suspend path
    int r = co_await inner_task(v);
    co_return r;
}

TEST_CASE("Task<int> co_await with await_ready false drives await_suspend", "[coroutine][task][regression]") {
    auto t = outer_await_ready(3);
    REQUIRE(t.get() == 6);
}

static Task<int> outer_await_done_inner() {
    // Create a task, run it to completion before co_await.
    // When the awaiter's await_ready fires, handle.done() is true.
    auto inner = inner_task(4);
    // Resume the inner task manually so it is done before co_await.
    while (!inner.done()) { inner.resume(); }
    // co_await a done task -> await_ready returns true -> skip suspend
    int r = co_await std::move(inner);
    co_return r;
}

TEST_CASE("Task<int> co_await on already-done task drives await_ready true", "[coroutine][task][regression]") {
    auto t = outer_await_done_inner();
    REQUIRE(t.get() == 8);
}

static Task<void> outer_void_await(Task<void> inner) {
    co_await std::move(inner);
}

TEST_CASE("Task<void> co_await drives FinalAwaiter for void specialization", "[coroutine][task][regression]") {
    auto inner = make_void_task();
    auto outer = outer_void_await(std::move(inner));
    outer.get();
    REQUIRE(outer.done());
}

static Task<void> outer_void_throwing() {
    co_await make_void_throwing_task();
}

TEST_CASE("Task<void> co_await rethrows exception through await_resume", "[coroutine][task][regression]") {
    auto t = outer_void_throwing();
    REQUIRE_THROWS_AS(t.get(), std::logic_error);
}

static Task<int> outer_int_throwing() {
    int v = co_await make_throwing_task();
    co_return v;
}

TEST_CASE("Task<int> co_await rethrows exception through await_resume", "[coroutine][task][regression]") {
    auto t = outer_int_throwing();
    REQUIRE_THROWS_AS(t.get(), std::runtime_error);
}

// ============================================================================
// Generator<T>
// ============================================================================

static Generator<int> gen_range(int start, int end) {
    for (int i = start; i < end; ++i) {
        co_yield i;
    }
}

static Generator<int> gen_empty() {
    co_return;
}

static Generator<int> gen_throwing() {
    co_yield 1;
    throw std::runtime_error("gen error");
}

TEST_CASE("Generator iterates values with range-for", "[coroutine][generator][regression]") {
    std::vector<int> got;
    for (int v : gen_range(0, 4)) {
        got.push_back(v);
    }
    REQUIRE(got.size() == 4);
    REQUIRE(got[0] == 0);
    REQUIRE(got[3] == 3);
}

TEST_CASE("Generator begin on empty (immediately done) returns end iterator", "[coroutine][generator][regression]") {
    auto g = gen_empty();
    auto b = g.begin();
    auto e = g.end();
    REQUIRE(b == e);
}

TEST_CASE("Generator Iterator equality: both-done and both-live", "[coroutine][generator][regression]") {
    auto g = gen_range(0, 2);
    auto b = g.begin();
    auto e = g.end();
    REQUIRE(b != e);      // b is live, e is sentinel (null handle)

    ++b;  // advance once
    ++b;  // advance to done
    REQUIRE(b == e);  // b is now done, equals sentinel
}

TEST_CASE("Generator next() returns values then nullopt when exhausted", "[coroutine][generator][regression]") {
    auto g = gen_range(10, 12);

    auto v0 = g.next();
    REQUIRE(v0.has_value());
    REQUIRE(*v0 == 10);

    auto v1 = g.next();
    REQUIRE(v1.has_value());
    REQUIRE(*v1 == 11);

    auto v2 = g.next();  // exhausted
    REQUIRE_FALSE(v2.has_value());

    // next() on an already-done generator also returns nullopt
    auto v3 = g.next();
    REQUIRE_FALSE(v3.has_value());
}

TEST_CASE("Generator next() on empty generator returns nullopt immediately", "[coroutine][generator][regression]") {
    auto g = gen_empty();
    auto v = g.next();
    REQUIRE_FALSE(v.has_value());
}

TEST_CASE("Generator iterator++ rethrows exception from body", "[coroutine][generator][regression]") {
    auto g = gen_throwing();
    auto it = g.begin();  // first resume yields 1
    REQUIRE(*it == 1);
    REQUIRE_THROWS_AS(++it, std::runtime_error);
}

TEST_CASE("Generator move assign with live handle", "[coroutine][generator][regression]") {
    auto g1 = gen_range(0, 3);
    auto g2 = gen_range(10, 13);
    g1 = std::move(g2);

    std::vector<int> got;
    for (int v : g1) {
        got.push_back(v);
    }
    REQUIRE(got.size() == 3);
    REQUIRE(got[0] == 10);
}

// ============================================================================
// SuspendIf: both predicate outcomes
// ============================================================================

static Task<int> task_with_suspend_if(bool should_suspend) {
    int val = 0;
    co_await suspend_if([&]() { return should_suspend; });
    val = 1;
    co_return val;
}

TEST_CASE("SuspendIf with predicate returning true actually suspends", "[coroutine][suspend_if][regression]") {
    auto t = task_with_suspend_if(true);
    // First resume starts the coroutine, hits suspend_if(true) -> suspends
    t.resume();
    REQUIRE_FALSE(t.done());
    // Second resume continues past suspend_if
    t.resume();
    REQUIRE(t.done());
    // Can't call get() after manual resumes without the exception/result pipeline;
    // just verify it completed without crash.
}

TEST_CASE("SuspendIf with predicate returning false does not suspend", "[coroutine][suspend_if][regression]") {
    auto t = task_with_suspend_if(false);
    // Single resume: suspend_if(false) -> await_ready=true -> no suspend
    t.resume();
    REQUIRE(t.done());
}

// ============================================================================
// ReadyAwaitable
// ============================================================================

static Task<int> task_using_ready_awaitable() {
    int v = co_await ready(42);
    co_return v;
}

TEST_CASE("ReadyAwaitable returns value immediately via co_await", "[coroutine][ready][regression]") {
    auto t = task_using_ready_awaitable();
    REQUIRE(t.get() == 42);
}
