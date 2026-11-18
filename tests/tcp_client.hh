#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace test_support {

// A minimal blocking TCP client for integration tests. Not meant to be
// efficient or general-purpose -- just enough to drive RESP commands
// against a real running server and read back replies.
class TcpClient {
public:
    TcpClient(const std::string& host, uint16_t port);
    ~TcpClient();

    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    void send(std::string_view data);

    // Reads until at least min_bytes have been received or the peer closes
    // the connection. May return more than min_bytes if the kernel buffered
    // extra data from a pipelined write.
    std::string recv_at_least(size_t min_bytes);

private:
    int fd_ = -1;
};

}  // namespace test_support
