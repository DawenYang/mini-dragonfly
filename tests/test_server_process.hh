#pragma once

#include <cstdint>

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

private:
    int pid_ = -1;
};

}  // namespace test_support
