#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "tcp_client.hh"
#include "test_server_process.hh"

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
    REQUIRE(client.recv_at_least(5) == "-ERR wrong number of arguments for 'ping' command\r\n");
}

TEST_CASE("ECHO replies with its argument as a bulk string", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$4\r\nECHO\r\n$5\r\nhello\r\n");
    REQUIRE(client.recv_at_least(11) == "$5\r\nhello\r\n");
}

TEST_CASE("ECHO with no argument is an error", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$4\r\nECHO\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR wrong number of arguments for 'echo' command\r\n");
}

TEST_CASE("DEL on a missing key returns :0", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$3\r\nDEL\r\n$7\r\nmissing\r\n");
    REQUIRE(client.recv_at_least(4) == ":0\r\n");
}

TEST_CASE("DEL removes the key and a subsequent GET misses", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$3\r\nDEL\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("DEL with multiple keys (possibly across shards) counts only those actually removed",
          "[server]") {
    TestServerProcess server(/*smp=*/4);
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$5\r\nalpha\r\n$1\r\n1\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");
    client.send("*3\r\n$3\r\nSET\r\n$5\r\nbravo\r\n$1\r\n2\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    // alpha and bravo exist, missing does not -- expect 2.
    client.send("*4\r\n$3\r\nDEL\r\n$5\r\nalpha\r\n$5\r\nbravo\r\n$7\r\nmissing\r\n");
    REQUIRE(client.recv_at_least(4) == ":2\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$5\r\nalpha\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
    client.send("*2\r\n$3\r\nGET\r\n$5\r\nbravo\r\n");
    REQUIRE(client.recv_at_least(5) == "$-1\r\n");
}

TEST_CASE("EXISTS on a missing key returns :0", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$6\r\nEXISTS\r\n$7\r\nmissing\r\n");
    REQUIRE(client.recv_at_least(4) == ":0\r\n");
}

TEST_CASE("EXISTS on a live key returns :1 without removing it", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$6\r\nEXISTS\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");

    client.send("*2\r\n$3\r\nGET\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(9) == "$3\r\nbar\r\n");
}

TEST_CASE("EXISTS counts the same key twice if it's given twice", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXISTS\r\n$3\r\nfoo\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(4) == ":2\r\n");
}

TEST_CASE("INCR on a missing key starts it at 1", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$4\r\nINCR\r\n$3\r\nctr\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");
}

TEST_CASE("INCR on an existing integer value adds one", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nctr\r\n$2\r\n41\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$4\r\nINCR\r\n$3\r\nctr\r\n");
    REQUIRE(client.recv_at_least(4) == ":42\r\n");
}

TEST_CASE("INCR on a non-integer value returns an error", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*2\r\n$4\r\nINCR\r\n$3\r\nfoo\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR value is not an integer or out of range\r\n");
}

TEST_CASE("COMMAND replies with an empty array stub", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$7\r\nCOMMAND\r\n");
    REQUIRE(client.recv_at_least(4) == "*0\r\n");
}

TEST_CASE("CONFIG GET replies with an empty array stub", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*2\r\n$6\r\nCONFIG\r\n$3\r\nGET\r\n");
    REQUIRE(client.recv_at_least(4) == "*0\r\n");
}

TEST_CASE("CONFIG SET replies with +OK", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*4\r\n$6\r\nCONFIG\r\n$3\r\nSET\r\n$9\r\nmaxmemory\r\n$3\r\n1gb\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");
}

TEST_CASE("CLIENT subcommands reply with +OK", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$6\r\nCLIENT\r\n$7\r\nSETINFO\r\n$7\r\nlib-ver\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");
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

TEST_CASE("an unrecognized command gets an error reply but the connection stays open", "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$4\r\nFROB\r\n");
    REQUIRE(client.recv_at_least(5) == "-ERR unknown command 'FROB'\r\n");

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

TEST_CASE("the server actively sweeps an expired key even if nothing ever touches it again",
          "[server]") {
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n");
    REQUIRE(client.recv_at_least(5) == "+OK\r\n");

    client.send("*3\r\n$6\r\nEXPIRE\r\n$3\r\nfoo\r\n$1\r\n0\r\n");
    REQUIRE(client.recv_at_least(4) == ":1\r\n");

    // Deliberately no GET/EXISTS/DEL on "foo" from here on -- the point is
    // that the server's background ExpirySweeper, not any client request
    // lazily expiring it, is what reclaims this key. Poll the server's own
    // log (captured by TestServerProcess) for its "removed" line instead
    // of ever asking about the key directly.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string log;
    while (std::chrono::steady_clock::now() < deadline) {
        log = server.read_log();
        if (log.find("expiry sweep removed") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(log.find("expiry sweep removed") != std::string::npos);
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

TEST_CASE("shutdown doesn't race live connections against a store that's being torn down",
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

TEST_CASE("a large pipelined batch in one write gets every reply back, in order", "[server]") {
    // Replies for a pipelined batch are buffered and flushed once, not
    // once per command (see parse_next_command()) -- this exercises that
    // at a scale where a broken or dropped flush would hang instead of
    // just being slow, since nothing else would ever prompt one.
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    constexpr int kCount = 500;
    std::string batch;
    for (int i = 0; i < kCount; ++i) {
        batch += "*1\r\n$4\r\nPING\r\n";
    }
    client.send(batch);

    std::string expected;
    for (int i = 0; i < kCount; ++i) {
        expected += "+PONG\r\n";
    }
    REQUIRE(client.recv_at_least(expected.size()) == expected);
}

TEST_CASE(
    "a trailing partial command left after a full one is compacted correctly before the next "
    "read completes it",
    "[server]") {
    // First write: one full PING, plus the start of a second one that's
    // deliberately left incomplete. The server parses and replies to the
    // first, leaving the partial second command sitting after it in buf_
    // with buf_pos_ > 0 -- exactly the case read_more_or_done() needs to
    // compact (erase the already-consumed prefix) before appending more,
    // rather than misinterpreting the leftover bytes' position.
    TestServerProcess server;
    TcpClient client("127.0.0.1", server.port());

    client.send("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPI");
    REQUIRE(client.recv_at_least(7) == "+PONG\r\n");

    client.send("NG\r\n");
    REQUIRE(client.recv_at_least(7) == "+PONG\r\n");
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
        const std::string cmd =
            "*2\r\n$3\r\nGET\r\n$" + std::to_string(key.size()) + "\r\n" + key + "\r\n";
        client.send(cmd);
        const std::string expected = "$" + std::to_string(value.size()) + "\r\n" + value + "\r\n";
        REQUIRE(client.recv_at_least(expected.size()) == expected);
    }
}

TEST_CASE("accept() failures (e.g. EMFILE) don't kill the accept loop permanently", "[server]") {
#ifdef MINI_DRAGONFLY_UNDER_SANITIZER
    // Seastar's io_uring reactor backend has a pre-existing UBSan
    // incompatibility specifically on the accept()-failure completion
    // path: do_process_ready_kernel_completions() ends up casting an
    // errored completion in a way UBSan flags as "member call on address
    // ... which does not point to an object of type 'kernel_completion'"
    // (invalid vptr). Every other test here only ever drives successful
    // completions and never hits it, but forcing accept() to fail is the
    // whole point of this one. Under this project's UBSAN_OPTIONS
    // (halt_on_error=1), that aborts the server outright -- unrelated to
    // the fix under test, and the same category of Seastar/sanitizer
    // incompatibility this project already chose not to chase for TSan.
    // The fix itself is still fully exercised under the plain build.
    SKIP(
        "accept()-failure path trips a pre-existing Seastar io_uring/UBSan "
        "incompatibility, unrelated to the fix under test");
#endif
    // Before the fix, accept_loop() used keep_doing(), which stops for
    // good on the first exception from accept() -- intentional shutdown
    // cancellation and a transient failure like running out of file
    // descriptors looked identical to it. Cap the server's own
    // RLIMIT_NOFILE low enough that it runs out of fds after accepting
    // just a couple of connections, forcing a real EMFILE/ENFILE out of
    // its accept() rather than simulating one.
    constexpr unsigned kFdLimit = 11;
    constexpr int kConnections = 15;
    TestServerProcess server(/*smp=*/1, kFdLimit);

    std::vector<std::unique_ptr<TcpClient>> held;
    for (int i = 0; i < kConnections; ++i) {
        held.push_back(std::make_unique<TcpClient>("127.0.0.1", server.port()));
    }

    // Give the accept loop a few of its backoff-and-retry cycles to run
    // into (and log) the fd exhaustion. Polled with a generous deadline
    // rather than a single fixed sleep -- under ASan plus MALLOC_PERTURB_
    // (as meson's test wrapper sets), everything is slow enough that a
    // short fixed wait isn't reliably enough time.
    const auto log_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::string log;
    while (std::chrono::steady_clock::now() < log_deadline) {
        log = server.read_log();
        if (log.find("accept() failed, will keep listening") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(log.find("accept() failed, will keep listening") != std::string::npos);

    // Release every held connection, freeing the fds the server was stuck
    // on, and give the accept loop a moment to notice on its next retry.
    held.clear();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // The real test: with the bug, accept_loop() died silently the first
    // time it hit EMFILE, so this fresh connection would never get
    // accepted and recv_at_least() would hang until SO_RCVTIMEO fires.
    TcpClient client("127.0.0.1", server.port());
    client.send("*1\r\n$4\r\nPING\r\n");
    REQUIRE(client.recv_at_least(7) == "+PONG\r\n");
}
