#include "test_server_process.hh"

#include "tcp_client.hh"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

namespace test_support {

namespace {

constexpr auto kReadyTimeout = std::chrono::seconds(5);
constexpr auto kReadyPollInterval = std::chrono::milliseconds(50);
constexpr auto kShutdownGrace = std::chrono::seconds(2);
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(20);

void wait_until_accepting(uint16_t port) {
    const auto deadline = std::chrono::steady_clock::now() + kReadyTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            TcpClient probe("127.0.0.1", port);
            return;  // Connected -- the server is up. Probe closes on scope exit.
        } catch (const std::runtime_error&) {
            std::this_thread::sleep_for(kReadyPollInterval);
        }
    }
    throw std::runtime_error("server did not start accepting connections on port " +
                              std::to_string(port) + " within the timeout");
}

}  // namespace

TestServerProcess::TestServerProcess(uint16_t port, unsigned smp) {
    const char* bin_path = std::getenv("MINI_DRAGONFLY_BIN");
    if (bin_path == nullptr) {
        throw std::runtime_error("MINI_DRAGONFLY_BIN is not set");
    }

    const std::string port_str = std::to_string(port);
    const std::string smp_str = std::to_string(smp);

    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        // Child: quiet the server's own log output so it doesn't interleave
        // with Catch2's own test output, then exec the real binary.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::execl(bin_path, bin_path, "--port", port_str.c_str(), "--smp", smp_str.c_str(),
                static_cast<char*>(nullptr));
        // execl only returns on failure.
        ::_exit(127);
    }

    pid_ = pid;
    wait_until_accepting(port);
}

TestServerProcess::~TestServerProcess() {
    if (pid_ < 0) {
        return;
    }

    ::kill(pid_, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + kShutdownGrace;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(pid_, &status, WNOHANG);
        if (r == pid_) {
            return;  // Exited cleanly (or at least, exited).
        }
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    // Didn't exit in time -- force it and reap unconditionally.
    ::kill(pid_, SIGKILL);
    int status = 0;
    ::waitpid(pid_, &status, 0);
}

}  // namespace test_support
