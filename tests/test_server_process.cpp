#include "test_server_process.hh"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#include "tcp_client.hh"

namespace test_support {

namespace {

constexpr auto kReadyTimeout = std::chrono::seconds(5);
constexpr auto kReadyPollInterval = std::chrono::milliseconds(50);
constexpr auto kShutdownGrace = std::chrono::seconds(2);
constexpr auto kShutdownPollInterval = std::chrono::milliseconds(20);

// Binds to port 0, letting the kernel assign a free one, reads back which
// port that was, then releases it again. There's an inherent TOCTOU race
// between this and the real server binding that same port a moment later
// -- see the something_listening() check right after this is called.
uint16_t find_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("bind() to port 0 failed: " + std::string(std::strerror(err)));
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        const int err = errno;
        ::close(fd);
        throw std::runtime_error("getsockname() failed: " + std::string(std::strerror(err)));
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}

bool something_listening(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool connected = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    ::close(fd);
    return connected;
}

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

TestServerProcess::TestServerProcess(unsigned smp, std::optional<unsigned> fd_limit)
    : port_(find_free_port()), log_path_(make_log_path()) {
    if (something_listening(port_)) {
        // The bind-then-close/exec window above raced with something else
        // grabbing the same port -- fail loudly here rather than silently
        // letting wait_until_accepting() below connect to that something
        // else and have the test talk to the wrong server.
        throw std::runtime_error("port " + std::to_string(port_) +
                                 " is unexpectedly already in use right before starting "
                                 "the test server");
    }

    const char* bin_path = std::getenv("MINI_DRAGONFLY_BIN");
    if (bin_path == nullptr) {
        throw std::runtime_error("MINI_DRAGONFLY_BIN is not set");
    }

    const std::string port_str = std::to_string(port_);
    const std::string smp_str = std::to_string(smp);

    const pid_t pid = ::fork();
    if (pid < 0) {
        throw std::runtime_error("fork() failed");
    }

    if (pid == 0) {
        // Child: quiet the server's own stdout (unused) and capture its
        // stderr (where its seastar::logger output goes) to log_path_
        // instead of letting it interleave with Catch2's own output. Done
        // before fd_limit is applied below -- these two small allocations
        // must not compete for the same tight budget fd_limit means to
        // impose on the server itself (e.g. under ASan, this process can
        // start with a couple of extra non-CLOEXEC fds already inherited
        // from the parent, leaving little headroom otherwise).
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

        if (fd_limit) {
            rlimit rl{};
            rl.rlim_cur = *fd_limit;
            rl.rlim_max = *fd_limit;
            if (::setrlimit(RLIMIT_NOFILE, &rl) != 0) {
                ::_exit(127);
            }
        }

        ::execl(bin_path, bin_path, "--port", port_str.c_str(), "--smp", smp_str.c_str(),
                static_cast<char*>(nullptr));
        // execl only returns on failure.
        ::_exit(127);
    }

    pid_ = pid;
    wait_until_accepting(port_);
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
