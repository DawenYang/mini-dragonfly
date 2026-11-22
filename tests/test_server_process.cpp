#include "test_server_process.hh"

#include "tcp_client.hh"

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::string make_log_path() {
    std::string tmpl = "/tmp/mini-dragonfly-test-log-XXXXXX";
    const int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        throw std::runtime_error("mkstemp() failed");
    }
    ::close(fd);
    return tmpl;
}

}  // namespace

TestServerProcess::TestServerProcess(uint16_t port, unsigned smp) : log_path_(make_log_path()) {
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
        // Child: quiet the server's own stdout (unused) and capture its
        // stderr (where its seastar::logger output goes) to log_path_
        // instead of letting it interleave with Catch2's own output.
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDOUT_FILENO);
            ::close(devnull);
        }
        const int log_fd = ::open(log_path_.c_str(), O_WRONLY | O_TRUNC);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDERR_FILENO);
            ::close(log_fd);
        }
        ::execl(bin_path, bin_path, "--port", port_str.c_str(), "--smp", smp_str.c_str(),
                static_cast<char*>(nullptr));
        // execl only returns on failure.
        ::_exit(127);
    }

    pid_ = pid;
    wait_until_accepting(port);
}

bool TestServerProcess::shutdown_and_wait() {
    if (pid_ < 0) {
        return true;
    }
    const pid_t pid = pid_;
    pid_ = -1;

    ::kill(pid, SIGTERM);

    const auto deadline = std::chrono::steady_clock::now() + kShutdownGrace;
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            return true;
        }
        std::this_thread::sleep_for(kShutdownPollInterval);
    }

    // Didn't exit in time -- force it and reap unconditionally.
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
    return false;
}

TestServerProcess::~TestServerProcess() {
    shutdown_and_wait();
    ::unlink(log_path_.c_str());
}

std::string TestServerProcess::read_log() const {
    std::ifstream f(log_path_);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace test_support
