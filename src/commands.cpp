#include "commands.hh"

#include <algorithm>
#include <cctype>
#include <optional>
#include <utility>

#include "parse_int.hh"
#include "resp.h"

namespace mini_dragonfly::commands {

namespace {

// PING's upper bound (at most one extra argument) can't be expressed by
// arity alone -- see CommandSpec::arity's comment -- so it's checked here,
// same as real Redis checks it inside its own pingCommand.
std::string ping_handler(ShardStore&, std::vector<std::string>& args) {
    if (args.size() > 2) {
        return "-ERR unknown command or wrong number of arguments\r\n";
    }
    if (args.size() == 2) {
        return resp::encode_bulk(std::optional(std::move(args[1])));
    }
    return "+PONG\r\n";
}

std::string echo_handler(ShardStore&, std::vector<std::string>& args) {
    return resp::encode_bulk(std::optional(std::move(args[1])));
}

// Doesn't touch the store at all -- answered directly on whichever shard
// the executor happened to run it on.
std::string command_handler(ShardStore&, std::vector<std::string>&) {
    return "*0\r\n";
}

std::string config_handler(ShardStore&, std::vector<std::string>& args) {
    if (args.size() >= 2 && to_upper(args[1]) == "GET") {
        return "*0\r\n";
    }
    return "+OK\r\n";
}

std::string client_handler(ShardStore&, std::vector<std::string>&) {
    return "+OK\r\n";
}

std::string get_handler(ShardStore& s, std::vector<std::string>& args) {
    return resp::encode_bulk(s.get(args[1]));
}

std::string set_handler(ShardStore& s, std::vector<std::string>& args) {
    s.set(std::move(args[1]), std::move(args[2]));
    return "+OK\r\n";
}

std::string expire_handler(ShardStore& s, std::vector<std::string>& args) {
    long long seconds = 0;
    if (!parse_integer(args[2], seconds)) {
        return "-ERR value is not an integer or out of range\r\n";
    }
    if (seconds < kMinExpirySeconds || seconds > kMaxExpirySeconds) {
        // ShardStore::expire() would clamp this safely rather than
        // overflow, but silently treating "expire in 10^18 seconds" as
        // "expire in ~292 years" without telling the caller is still a
        // surprise worth rejecting outright.
        return "-ERR invalid expire time\r\n";
    }
    return resp::encode_integer(s.expire(args[1], seconds) ? 1 : 0);
}

std::string incr_handler(ShardStore& s, std::vector<std::string>& args) {
    const ShardStore::IncrOutcome outcome = s.incr(args[1]);
    if (outcome.status == ShardStore::IncrOutcome::Status::NotInteger) {
        return "-ERR value is not an integer or out of range\r\n";
    }
    if (outcome.status == ShardStore::IncrOutcome::Status::Overflow) {
        return "-ERR increment or decrement would overflow\r\n";
    }
    return resp::encode_integer(outcome.value);
}

bool del_handler(ShardStore& s, const std::string& key) {
    return s.del(key);
}

bool exists_handler(ShardStore& s, const std::string& key) {
    return s.exists(key);
}

}  // namespace

const std::unordered_map<std::string, CommandSpec>& table() {
    static const std::unordered_map<std::string, CommandSpec> table = {
        {"PING", {-1, 0, 0, 0, LockMode::None, Handler{ping_handler}}},
        {"ECHO", {2, 0, 0, 0, LockMode::None, Handler{echo_handler}}},
        {"COMMAND", {-1, 0, 0, 0, LockMode::None, Handler{command_handler}}},
        {"CONFIG", {-1, 0, 0, 0, LockMode::None, Handler{config_handler}}},
        {"CLIENT", {-1, 0, 0, 0, LockMode::None, Handler{client_handler}}},
        {"GET", {2, 1, 1, 1, LockMode::Read, Handler{get_handler}}},
        {"SET", {3, 1, 1, 1, LockMode::Write, Handler{set_handler}}},
        {"EXPIRE", {3, 1, 1, 1, LockMode::Write, Handler{expire_handler}}},
        {"INCR", {2, 1, 1, 1, LockMode::Write, Handler{incr_handler}}},
        {"DEL", {-2, 1, -1, 1, LockMode::Write, MultiKeyHandler{del_handler}}},
        {"EXISTS", {-2, 1, -1, 1, LockMode::Read, MultiKeyHandler{exists_handler}}},
    };
    return table;
}

bool arity_matches(int arity, size_t n) {
    const auto signed_n = static_cast<int>(n);
    return arity >= 0 ? signed_n == arity : signed_n >= -arity;
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

}  // namespace mini_dragonfly::commands
