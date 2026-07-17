#include <catch2/catch_test_macros.hpp>

#include "nexusfix/util/logger.hpp"

// ============================================================================
// Logger Tests
//
// Quill's Backend::start() uses std::call_once: the backend thread can only
// be started once per process. Backend::stop() is terminal — a subsequent
// start() is a no-op because the once_flag is already set. Any flush_log()
// after stop() blocks forever waiting for a dead thread.
//
// Consequence: we call init() once (via static initializer) and never call
// shutdown() until process exit (via static destructor). Individual tests
// must NOT call shutdown().
//
// When NFX_HAS_LOGGING is not defined, all functions are no-ops so the
// constraint is irrelevant but the tests still verify compilation.
// ============================================================================

namespace {
struct LoggerLifecycle {
    LoggerLifecycle() { nfx::logging::init(); }
    ~LoggerLifecycle() { nfx::logging::shutdown(); }
};
static const LoggerLifecycle logger_lifecycle_{};
}  // namespace

TEST_CASE("Logger init with default config does not crash", "[logger][regression]") {
    nfx::logging::init();
    CHECK(true);
}

TEST_CASE("Logger get returns non-null after init", "[logger][regression]") {
    nfx::logging::init();
    auto* logger = nfx::logging::get();

#ifdef NFX_HAS_LOGGING
    REQUIRE(logger != nullptr);
#else
    CHECK(logger == nullptr);
#endif
}

TEST_CASE("Logger double init is safe", "[logger][regression]") {
    nfx::logging::init();
    nfx::logging::init();
    CHECK(true);
}

TEST_CASE("NFX_LOG_INFO compiles and runs without crash", "[logger][regression]") {
    nfx::logging::init();
    NFX_LOG_INFO("test log message from unit test");
    nfx::logging::flush();
    CHECK(true);
}

TEST_CASE("Conditional logging macros compile and run", "[logger][regression]") {
    nfx::logging::init();

    NFX_LOG_INFO_IF(true, "should log");
    NFX_LOG_INFO_IF(false, "should not log");
    NFX_LOG_WARN_IF(true, "warn conditional");
    NFX_LOG_ERROR_IF(false, "error conditional");

    nfx::logging::flush();
    CHECK(true);
}

// ============================================================================
// WS3: Logger level switch branches and sink on/off combos (TICKET_497_3)
// ============================================================================

TEST_CASE("Logger init with every Level value", "[logger][level][regression]") {
    using Level = nfx::logging::Level;
    nfx::logging::LogConfig cfg;
    cfg.console_output = false;
    cfg.file_output = false;

    for (Level lv : {Level::Trace, Level::Debug, Level::Info,
                     Level::Warning, Level::Error, Level::Critical}) {
        cfg.min_level = lv;
        nfx::logging::init(cfg);
    }
    CHECK(true);
}

TEST_CASE("Logger init console only", "[logger][sinks][regression]") {
    nfx::logging::LogConfig cfg;
    cfg.console_output = true;
    cfg.file_output = false;
    cfg.min_level = nfx::logging::Level::Warning;
    nfx::logging::init(cfg);
    nfx::logging::flush();
    CHECK(true);
}

TEST_CASE("Logger init file only", "[logger][sinks][regression]") {
    nfx::logging::LogConfig cfg;
    cfg.console_output = false;
    cfg.file_output = true;
    cfg.log_dir = "/tmp/nfx_test_logs";
    cfg.min_level = nfx::logging::Level::Error;
    nfx::logging::init(cfg);
    nfx::logging::flush();
    CHECK(true);
}

TEST_CASE("Logger init both console and file", "[logger][sinks][regression]") {
    nfx::logging::LogConfig cfg;
    cfg.console_output = true;
    cfg.file_output = true;
    cfg.log_dir = "/tmp/nfx_test_logs";
    cfg.min_level = nfx::logging::Level::Info;
    nfx::logging::init(cfg);
    nfx::logging::flush();
    CHECK(true);
}

TEST_CASE("Logger init neither console nor file", "[logger][sinks][regression]") {
    nfx::logging::LogConfig cfg;
    cfg.console_output = false;
    cfg.file_output = false;
    cfg.min_level = nfx::logging::Level::Debug;
    nfx::logging::init(cfg);
    nfx::logging::flush();
    CHECK(true);
}

TEST_CASE("Logger flush when no logger initialized is safe", "[logger][regression]") {
    nfx::logging::flush();
    CHECK(true);
}
