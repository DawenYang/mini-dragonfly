#pragma once

#include <cstdint>
#include <string>

namespace test_support {

// Launches the real mini-dragonfly binary as a child process for the
// duration of this object's lifetime, and blocks in the constructor until
// it's actually accepting connections on the given port. Sends SIGTERM and
// waits for a clean exit in the destructor (escalating to SIGKILL if the
// child doesn't exit promptly).
//
// The binary's path is read from the MINI_DRAGONFLY_BIN environment
// variable, which meson.build sets when registering this test so the
// absolute build path doesn't need to be hardcoded or guessed.
class TestServerProcess {
public:
    explicit TestServerProcess(uint16_t port, unsigned smp = 1);
    ~TestServerProcess();

    TestServerProcess(const TestServerProcess&) = delete;
    TestServerProcess& operator=(const TestServerProcess&) = delete;

    // Sends SIGTERM and waits for the process to exit, escalating to
    // SIGKILL if it doesn't within a grace period. Returns true if it
    // exited on its own (a clean, prompt shutdown), false if SIGKILL was
    // needed. Safe to call before destruction -- the destructor becomes a
    // no-op if this has already run.
    bool shutdown_and_wait();

    // The server's own log output (its seastar::logger writes to stderr,
    // captured here to a temp file for the process's lifetime) so tests
    // can check for unexpected warnings or errors -- e.g. a connection
    // still trying to use a store that's being torn down mid-shutdown.
    std::string read_log() const;

private:
    int pid_ = -1;
    std::string log_path_;
};

}  // namespace test_support
