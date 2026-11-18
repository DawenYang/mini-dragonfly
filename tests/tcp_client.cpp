#include "tcp_client.hh"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace test_support {

TcpClient::TcpClient(const std::string& host, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error("socket() failed: " + std::string(std::strerror(errno)));
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd_);
        throw std::runtime_error("inet_pton() failed for host: " + host);
    }

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(fd_);
        throw std::runtime_error("connect() failed: " + std::string(std::strerror(err)));
    }
}

TcpClient::~TcpClient() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void TcpClient::send(std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            throw std::runtime_error("send() failed: " + std::string(std::strerror(errno)));
        }
        sent += static_cast<size_t>(n);
    }
}

std::string TcpClient::recv_at_least(size_t min_bytes) {
    std::string result;
    char buf[4096];
    while (result.size() < min_bytes) {
        const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n < 0) {
            throw std::runtime_error("recv() failed: " + std::string(std::strerror(errno)));
        }
        if (n == 0) {
            break;  // Peer closed the connection.
        }
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

}  // namespace test_support
