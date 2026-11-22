#include "commands.hh"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using mini_dragonfly::ShardStore;
using mini_dragonfly::commands::arity_matches;
using mini_dragonfly::commands::CommandSpec;
using mini_dragonfly::commands::Handler;
using mini_dragonfly::commands::LockMode;
using mini_dragonfly::commands::MultiKeyHandler;
using mini_dragonfly::commands::table;
using mini_dragonfly::commands::to_upper;

namespace {

// Returns by value (CommandSpec is a handful of ints, an enum, and a
// 2-pointer variant -- cheap to copy) rather than by reference: a
// reference out of here binding to a string-literal argument's temporary
// std::string trips GCC's (overly cautious, but not unreasonable to just
// avoid) -Wdangling-reference.
CommandSpec spec_for(const std::string& name) {
    const auto& t = table();
    const auto it = t.find(name);
    REQUIRE(it != t.end());
    return it->second;
}

// Calls a single-key (or keyless) command's handler directly, the same
// way Connection::execute() does in server.cpp, without any Seastar
// involvement.
std::string run(const std::string& name, std::vector<std::string> args, ShardStore& store) {
    const Handler handler = std::get<Handler>(spec_for(name).handler);
    return handler(store, args);
}

// Calls a multi-key command's handler for one key, the same way
// Connection::execute() does per key in its multi-key path.
bool run_multi(const std::string& name, const std::string& key, ShardStore& store) {
    const MultiKeyHandler handler = std::get<MultiKeyHandler>(spec_for(name).handler);
    return handler(store, key);
}

}  // namespace

TEST_CASE("to_upper uppercases ASCII letters and leaves everything else alone", "[commands]") {
    REQUIRE(to_upper("get") == "GET");
    REQUIRE(to_upper("Get") == "GET");
    REQUIRE(to_upper("") == "");
}

TEST_CASE("arity_matches follows the Redis exact/minimum convention", "[commands]") {
    // Positive: exact.
    REQUIRE(arity_matches(3, 3));
    REQUIRE_FALSE(arity_matches(3, 2));
    REQUIRE_FALSE(arity_matches(3, 4));
    // Negative: at least |arity|.
    REQUIRE(arity_matches(-2, 2));
    REQUIRE(arity_matches(-2, 5));
    REQUIRE_FALSE(arity_matches(-2, 1));
}

TEST_CASE("the table's single-key commands declare first_key == last_key with LockMode set",
          "[commands]") {
    const auto& get_spec = spec_for("GET");
    REQUIRE(get_spec.first_key == 1);
    REQUIRE(get_spec.last_key == 1);
    REQUIRE(get_spec.key_step == 1);
    REQUIRE(get_spec.lock_mode == LockMode::Read);

    const auto& set_spec = spec_for("SET");
    REQUIRE(set_spec.lock_mode == LockMode::Write);
}

TEST_CASE("the table's multi-key commands declare last_key == -1", "[commands]") {
    const auto& del_spec = spec_for("DEL");
    REQUIRE(del_spec.first_key == 1);
    REQUIRE(del_spec.last_key == -1);
    REQUIRE(del_spec.key_step == 1);
    REQUIRE(del_spec.lock_mode == LockMode::Write);

    const auto& exists_spec = spec_for("EXISTS");
    REQUIRE(exists_spec.lock_mode == LockMode::Read);
}

TEST_CASE("keyless commands declare first_key == 0 and LockMode::None", "[commands]") {
    for (const std::string name : {"PING", "ECHO", "COMMAND", "CONFIG", "CLIENT"}) {
        const auto& spec = spec_for(name);
        REQUIRE(spec.first_key == 0);
        REQUIRE(spec.lock_mode == LockMode::None);
    }
}

TEST_CASE("PING with no argument replies with +PONG", "[commands]") {
    ShardStore store;
    REQUIRE(run("PING", {"PING"}, store) == "+PONG\r\n");
}

TEST_CASE("PING with an argument echoes it back as a bulk string", "[commands]") {
    ShardStore store;
    REQUIRE(run("PING", {"PING", "hello"}, store) == "$5\r\nhello\r\n");
}

TEST_CASE("PING with more than one argument is an error", "[commands]") {
    // arity (-1, "at least 1") can't express PING's upper bound of 2, so
    // the handler itself rejects anything past that -- see its comment in
    // commands.cpp.
    ShardStore store;
    REQUIRE(run("PING", {"PING", "a", "b"}, store) ==
            "-ERR unknown command or wrong number of arguments\r\n");
}

TEST_CASE("ECHO replies with its argument as a bulk string", "[commands]") {
    ShardStore store;
    REQUIRE(run("ECHO", {"ECHO", "hi"}, store) == "$2\r\nhi\r\n");
}

TEST_CASE("GET on a missing key returns the null bulk string", "[commands]") {
    ShardStore store;
    REQUIRE(run("GET", {"GET", "missing"}, store) == "$-1\r\n");
}

TEST_CASE("SET then GET round-trips the value", "[commands]") {
    ShardStore store;
    REQUIRE(run("SET", {"SET", "foo", "bar"}, store) == "+OK\r\n");
    REQUIRE(run("GET", {"GET", "foo"}, store) == "$3\r\nbar\r\n");
}

TEST_CASE("EXPIRE on a missing key returns :0", "[commands]") {
    ShardStore store;
    REQUIRE(run("EXPIRE", {"EXPIRE", "missing", "10"}, store) == ":0\r\n");
}

TEST_CASE("EXPIRE on an existing key returns :1", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run("EXPIRE", {"EXPIRE", "foo", "10"}, store) == ":1\r\n");
}

TEST_CASE("EXPIRE with a non-integer seconds argument is an error", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run("EXPIRE", {"EXPIRE", "foo", "abc"}, store) ==
            "-ERR value is not an integer or out of range\r\n");
}

TEST_CASE("EXPIRE with an absurdly large seconds value is rejected", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run("EXPIRE", {"EXPIRE", "foo", "99999999999"}, store) ==
            "-ERR invalid expire time\r\n");
}

TEST_CASE("INCR on a missing key starts it at 1", "[commands]") {
    ShardStore store;
    REQUIRE(run("INCR", {"INCR", "ctr"}, store) == ":1\r\n");
}

TEST_CASE("INCR on an existing integer value adds one", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "ctr", "41"}, store);
    REQUIRE(run("INCR", {"INCR", "ctr"}, store) == ":42\r\n");
}

TEST_CASE("INCR on a non-integer value is an error", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run("INCR", {"INCR", "foo"}, store) ==
            "-ERR value is not an integer or out of range\r\n");
}

TEST_CASE("DEL's per-key handler removes a live key and returns true", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run_multi("DEL", "foo", store));
    REQUIRE(run("GET", {"GET", "foo"}, store) == "$-1\r\n");
}

TEST_CASE("DEL's per-key handler on a missing key returns false", "[commands]") {
    ShardStore store;
    REQUIRE_FALSE(run_multi("DEL", "missing", store));
}

TEST_CASE("EXISTS's per-key handler reflects liveness without removing the key", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run_multi("EXISTS", "foo", store));
    REQUIRE(run("GET", {"GET", "foo"}, store) == "$3\r\nbar\r\n");
    REQUIRE_FALSE(run_multi("EXISTS", "missing", store));
}

TEST_CASE("COMMAND replies with an empty array stub", "[commands]") {
    ShardStore store;
    REQUIRE(run("COMMAND", {"COMMAND"}, store) == "*0\r\n");
}

TEST_CASE("CONFIG GET replies with an empty array stub", "[commands]") {
    ShardStore store;
    REQUIRE(run("CONFIG", {"CONFIG", "GET"}, store) == "*0\r\n");
}

TEST_CASE("CONFIG SET replies with +OK", "[commands]") {
    ShardStore store;
    REQUIRE(run("CONFIG", {"CONFIG", "SET", "maxmemory", "1gb"}, store) == "+OK\r\n");
}

TEST_CASE("CLIENT subcommands reply with +OK", "[commands]") {
    ShardStore store;
    REQUIRE(run("CLIENT", {"CLIENT", "SETINFO", "lib-name", "x"}, store) == "+OK\r\n");
}
