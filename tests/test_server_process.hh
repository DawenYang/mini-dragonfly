#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace test_support {

// Launches the real mini-dragonfly binary as a child process for the
// duration of this object's lifetime, listening on a freshly chosen free
// port (see port()) rather than a fixed one -- so that several instances
// of this class can coexist, e.g. under `meson test --repeat`, which runs
// copies of the whole test binary in parallel. A fixed port would make the
// second copy's server fail to bind, and the first copy's tests would then
// unknowingly talk to whichever server actually ended up listening.
//
// Blocks in the constructor until the server is actually accepting
// connections on that port. Sends SIGTERM and waits for a clean exit in
// the destructor (escalating to SIGKILL if the child doesn't exit
// promptly).
//
// The binary's path is read from the MINI_DRAGONFLY_BIN environment
// variable, which meson.build sets when registering this test so the
// absolute build path doesn't need to be hardcoded or guessed.
class TestServerProcess {
public:
    // fd_limit, if set, caps the child's RLIMIT_NOFILE before it execs --
    // used to deterministically provoke an EMFILE/ENFILE from accept()
    // under test, rather than needing to actually exhaust the real
    // process's file descriptors.
    explicit TestServerProcess(unsigned smp = 1, std::optional<unsigned> fd_limit = std::nullopt);
    ~TestServerProcess();

    TestServerProcess(const TestServerProcess&) = delete;
    TestServerProcess& operator=(const TestServerProcess&) = delete;

    uint16_t port() const noexcept { return port_; }

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
    uint16_t port_ = 0;
    int pid_ = -1;
    std::string log_path_;
};

}  // namespace test_support
