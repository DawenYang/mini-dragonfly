#include "tcp_client.hh"
#include "test_server_process.hh"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using test_support::TcpClient;
using test_support::TestServerProcess;

TEST_CASE("PING with no argument replies with +PONG", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$4\r\nPING\r\n");
    REQUIRE(client.recv_at_least(7) == "+PONG\r\n");
}

TEST_CASE("PING with an argument echoes it back as a bulk string", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$4\r\nPING\r\n$5\r\nhello\r\n");
    REQUIRE(client.recv_at_least(11) == "$5\r\nhello\r\n");
}

TEST_CASE("PING with more than one argument is an error", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$4\r\nPING\r\n$1\r\na\r\n$1\r\nb\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR unknown command or wrong number of arguments\r\n");
}

TEST_CASE("SET then GET round-trips the value", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(9) == "$3\r\nbar\r\n");
}

TEST_CASE("GET on a key that was never set returns $-1", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$3\r\nGET\r\n$7\r\nmissing\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("an unrecognized command gets an error reply but the connection stays open",
          "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$4\r\nFROB\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR unknown command or wrong number of arguments\r\n");

    // The connection should still be usable afterward -- an unknown command
    // isn't a protocol error, it just doesn't match anything.
    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");
}

TEST_CASE("EXPIRE on a missing key returns :0", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$6\r\nEXPIRE\r\n$7\r\nmissing\r\n$2\r\n10\r\n");
    REQUIRE(client.recv_at_least(4) == ":0\r\n");
}

TEST_CASE("EXPIRE on an existing key returns :1, and a zero-second expiry takes effect",
          "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$1\r\n0\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("EXPIRE with a non-integer seconds argument returns an error", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$3\r\nabc\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR value is not an integer or out of range\r\n");
}

TEST_CASE("EXPIRE with an absurdly large seconds value is rejected, not misinterpreted",
          "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    // Large enough to overflow steady_clock's nanosecond-resolution
    // arithmetic if not guarded against -- previously this expired the
    // key immediately instead of being rejected.
    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$11\r\n99999999999\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR invalid expire time\r\n");

    // foo must still be there -- the rejected EXPIRE should not have
    // touched it.
    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(9) == "$3\r\nbar\r\n");
}

TEST_CASE("shutdown closes a live, idle connection instead of hanging", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    // Deliberately leave the connection open and idle -- no request sent,
    // nothing closed from this end. Before the fix, TcpServer::stop() only
    // tracked the accept loop: this connection's fiber would stay blocked
    // forever on in_.read(), so the process would never exit on its own
    // and shutdown_and_wait() would have to fall back to SIGKILL.
    REQUIRE(server.shutdown_and_wait());
}

TEST_CASE(
    "shutdown doesn't race live connections against a store that's being torn down",
    "[server]") {
    // Before the fix, TcpServer::stop() didn't track or wait for
    // in-flight connections: it returned as soon as the accept loop
    // stopped, letting main() immediately tear down the sharded<ShardStore>
    // while connections on other shards could still be mid-invoke_on()
    // into it. Reproducing that reliably needs genuine concurrent traffic
    // across shards at the exact moment of shutdown, which is why this
    // drives real commands from several threads rather than one sequential
    // client.
    constexpr unsigned kSmp = 4;
    constexpr int kWorkers = 16;
    TestServerProcess server(kSmp);
    const uint16_t port = server.port();

    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < kWorkers; ++i) {
        workers.emplace_back([i, port, &stop] {
            try {
                TcpClient client("127.0.0.1", port);
                const std::string key = "key" + std::to_string(i);
                const std::string cmd = "*3\r\n$3\r\nSET\r\n$" + std::to_string(key.size()) +
                                         "\r\n" + key + "\r\n$3\r\nbar\r\n";
                while (!stop.load(std::memory_order_relaxed)) {
                    client.send(cmd);
                    client.recv_at_least(5);
                }
            } catch (const std::runtime_error&) {
                // Expected once the server goes down mid-request.
            }
        });
    }

    // Give the workers time to get requests genuinely in flight across
    // shards before shutting down underneath them.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool shut_down_cleanly = server.shutdown_and_wait();

    stop = true;
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE(shut_down_cleanly);

    const std::string log = server.read_log();
    INFO(log);
    // These are the two exceptions a connection hits when it calls into a
    // sharded<ShardStore> that's already been (or is being) torn down --
    // their absence is what the fix guarantees.
    REQUIRE(log.find("no_sharded_instance_exception") == std::string::npos);
    REQUIRE(log.find("gate_closed_exception") == std::string::npos);
}

TEST_CASE("a malformed frame gets a protocol error and the connection closes", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("GARBAGE\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR protocol error\r\n");

    // The server should have closed its end after the protocol error -- a
    // further read returns empty (eof) rather than hanging or erroring.
    REQUIRE(client.recv_at_least(1).empty());
}

TEST_CASE("pipelined commands on one write are each answered in order", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send(
        "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n"
        "*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5 + 9) == "+OK\r\n$3\r\nbar\r\n");
}

TEST_CASE("GET and SET work correctly across multiple shards", "[server]") {
    // With smp > 1, different keys hash to different owning shards (see
    // owner_shard() in store.cpp). This exercises the cross-shard
    // sharded<ShardStore>::invoke_on() path, not just the single-shard case.
    TestServerProcess server(/*smp=*/4);
    TcpClient client("127.0.0.1", server.port());

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
