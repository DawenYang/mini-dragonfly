#include "tcp_client.hh"
#include "test_server_process.hh"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using test_support::TcpClient;
using test_support::TestServerProcess;

namespace {
constexpr uint16_t kTestPort = 17380;
}  // namespace

TEST_CASE("SET then GET round-trips the value", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(9) == "$3\r\nbar\r\n");
}

TEST_CASE("GET on a key that was never set returns $-1", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("an unrecognized command gets an error reply but the connection stays open",
          "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*1\r\n$4\r\nPING\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR unknown command or wrong number of arguments\r\n");

    // The connection should still be usable afterward -- an unknown command
    // isn't a protocol error, it just doesn't match anything.
    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");
}

TEST_CASE("EXPIRE on a missing key returns :0", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*3\r\n$6\r\nEXPIRE\r\n$7\r\nmissing\r\n$2\r\n10\r\n");
    REQUIRE(client.recv_at_least(4) == ":0\r\n");
}

TEST_CASE("EXPIRE on an existing key returns :1, and a zero-second expiry takes effect",
          "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$1\r\n0\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("EXPIRE with a non-integer seconds argument returns an error", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$3\r\nabc\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR value is not an integer or out of range\r\n");
}

TEST_CASE("a malformed frame gets a protocol error and the connection closes", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send("GARBAGE\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR protocol error\r\n");

    // The server should have closed its end after the protocol error -- a
    // further read returns empty (eof) rather than hanging or erroring.
    REQUIRE(client.recv_at_least(1).empty());
}

TEST_CASE("pipelined commands on one write are each answered in order", "[server]") {
    TestServerProcess server(kTestPort);
    TcpClient client("127.0.0.1", kTestPort);

    client.send(
        "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5 + 9) == "+OK\r\n$3\r\nbar\r\n");
}

TEST_CASE("GET and SET work correctly across multiple shards", "[server]") {
    // With smp > 1, different keys hash to different owning shards (see
    // owner_shard() in store.cpp). This exercises the cross-shard
    // sharded<ShardStore>::invoke_on() path, not just the single-shard case.
    TestServerProcess server(kTestPort, /*smp=*/4);
    TcpClient client("127.0.0.1", kTestPort);

    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"alpha", "1"}, {"bravo", "2"}, {"charlie", "3"}, {"delta", "4"}, {"echo", "5"},
    };

    for (const auto& [key, value] : pairs) {
        const std::string cmd = "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) + "\r\n" + key +
                                 "\r\n$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        client.send(cmd);
        REQUIRE(client.recv_at_least(5) == "+OK\r\n");
    }

    for (const auto& [key, value] : pairs) {
        const std::string cmd = "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
        client.send(cmd);
        const std::string expected =
            "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        REQUIRE(client.recv_at_least(expected.size()) == expected);
    }
}
