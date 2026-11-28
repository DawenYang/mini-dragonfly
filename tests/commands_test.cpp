#include "commands.hh"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

using mini_dragonfly::LockMode;
using mini_dragonfly::ShardStore;
using mini_dragonfly::commands::arity_matches;
using mini_dragonfly::commands::CommandSpec;
using mini_dragonfly::commands::Handler;
using mini_dragonfly::commands::key_indices;
using mini_dragonfly::commands::KeyResult;
using mini_dragonfly::commands::MultiKeyHandler;
using mini_dragonfly::commands::table;
using mini_dragonfly::commands::to_upper;
using mini_dragonfly::commands::unknown_command_error;
using mini_dragonfly::commands::wrong_arity_error;

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
// way execute_command() does, without any Seastar
// involvement.
std::string run(const std::string& name, std::vector<std::string> args, ShardStore& store) {
    const Handler handler = std::get<Handler>(spec_for(name).handler);
    return handler(store, args);
}

// Runs a multi-key command the way execute_command() does, with every key
// on the one given store.
std::string run_multi(const std::string& name, std::vector<std::string> args, ShardStore& store) {
    const CommandSpec spec = spec_for(name);
    const MultiKeyHandler handler = std::get<MultiKeyHandler>(spec.handler);
    std::vector<KeyResult> results;
    for (const size_t i : key_indices(spec, args.size())) {
        results.push_back(handler.per_key(store, args, i));
    }
    return handler.combine(std::move(results));
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
            "-ERR wrong number of arguments for 'ping' command\r\n");
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

TEST_CASE("DEL removes live keys and counts only those", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run_multi("DEL", {"DEL", "foo", "missing", "foo"}, store) == ":1\r\n");
    REQUIRE(run("GET", {"GET", "foo"}, store) == "$-1\r\n");
}

TEST_CASE("EXISTS counts live keys without removing them", "[commands]") {
    ShardStore store;
    run("SET", {"SET", "foo", "bar"}, store);
    REQUIRE(run_multi("EXISTS", {"EXISTS", "foo", "missing", "foo"}, store) == ":2\r\n");
    REQUIRE(run("GET", {"GET", "foo"}, store) == "$3\r\nbar\r\n");
}

TEST_CASE("MSET sets every pair and MGET returns them in order", "[commands]") {
    ShardStore store;
    REQUIRE(run_multi("MSET", {"MSET", "a", "1", "b", "", "a", "3"}, store) == "+OK\r\n");
    REQUIRE(run_multi("MGET", {"MGET", "b", "missing", "a"}, store) ==
            "*3\r\n$0\r\n\r\n$-1\r\n$1\r\n3\r\n");
}

TEST_CASE("MSET rejects a key without a value", "[commands]") {
    const CommandSpec spec = spec_for("MSET");
    REQUIRE_FALSE(arity_matches(spec, 1));
    REQUIRE_FALSE(arity_matches(spec, 2));
    REQUIRE(arity_matches(spec, 3));
    REQUIRE_FALSE(arity_matches(spec, 4));
    REQUIRE(arity_matches(spec, 5));
}

TEST_CASE("key_indices follows first_key, last_key and key_step", "[commands]") {
    REQUIRE(key_indices(spec_for("PING"), 2).empty());
    REQUIRE(key_indices(spec_for("SET"), 3) == std::vector<size_t>{1});
    REQUIRE(key_indices(spec_for("DEL"), 4) == std::vector<size_t>{1, 2, 3});
    REQUIRE(key_indices(spec_for("MSET"), 5) == std::vector<size_t>{1, 3});
}

TEST_CASE("exactly the commands with keys declare a lock mode", "[commands]") {
    for (const auto& [name, spec] : table()) {
        INFO(name);
        REQUIRE((spec.first_key == 0) == (spec.lock_mode == LockMode::None));
    }
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

TEST_CASE("every keyed table entry has a key_step of at least 1", "[commands]") {
    // execute() advances through multi-key args by key_step; 0 would loop
    // forever.
    for (const auto& [name, spec] : table()) {
        if (spec.first_key != 0) {
            INFO(name);
            REQUIRE(spec.key_step >= 1);
        }
    }
}

TEST_CASE("every table entry's key range and handler alternative agree", "[commands]") {
    for (const auto& [name, spec] : table()) {
        INFO(name);
        if (spec.first_key == 0) {
            REQUIRE(std::holds_alternative<Handler>(spec.handler));
        } else if (spec.last_key == -1) {
            REQUIRE(std::holds_alternative<MultiKeyHandler>(spec.handler));
        } else {
            REQUIRE(spec.last_key >= spec.first_key);
            REQUIRE(std::holds_alternative<Handler>(spec.handler));
        }
    }
}

TEST_CASE("error replies match what Redis clients expect", "[commands]") {
    REQUIRE(unknown_command_error("foo") == "-ERR unknown command 'foo'\r\n");
    REQUIRE(wrong_arity_error("GeT") == "-ERR wrong number of arguments for 'get' command\r\n");
}

TEST_CASE("error replies can't be broken by CR/LF or huge names in the echoed command name",
          "[commands]") {
    const std::string reply = unknown_command_error("a\r\n+OK\r\nb");
    REQUIRE(reply == "-ERR unknown command 'a  +OK  b'\r\n");

    const std::string long_reply = unknown_command_error(std::string(100000, 'x'));
    REQUIRE(long_reply.size() < 200);
}
