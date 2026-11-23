#include "commands.hh"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>
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
        return wrong_arity_error("ping");
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
        {"PING",
         {.arity = -1,
          .first_key = 0,
          .last_key = 0,
          .key_step = 0,
          .lock_mode = LockMode::None,
          .handler = Handler{ping_handler}}},
        {"ECHO",
         {.arity = 2,
          .first_key = 0,
          .last_key = 0,
          .key_step = 0,
          .lock_mode = LockMode::None,
          .handler = Handler{echo_handler}}},
        {"COMMAND",
         {.arity = -1,
          .first_key = 0,
          .last_key = 0,
          .key_step = 0,
          .lock_mode = LockMode::None,
          .handler = Handler{command_handler}}},
        {"CONFIG",
         {.arity = -1,
          .first_key = 0,
          .last_key = 0,
          .key_step = 0,
          .lock_mode = LockMode::None,
          .handler = Handler{config_handler}}},
        {"CLIENT",
         {.arity = -1,
          .first_key = 0,
          .last_key = 0,
          .key_step = 0,
          .lock_mode = LockMode::None,
          .handler = Handler{client_handler}}},
        {"GET",
         {.arity = 2,
          .first_key = 1,
          .last_key = 1,
          .key_step = 1,
          .lock_mode = LockMode::Read,
          .handler = Handler{get_handler}}},
        {"SET",
         {.arity = 3,
          .first_key = 1,
          .last_key = 1,
          .key_step = 1,
          .lock_mode = LockMode::Write,
          .handler = Handler{set_handler}}},
        {"EXPIRE",
         {.arity = 3,
          .first_key = 1,
          .last_key = 1,
          .key_step = 1,
          .lock_mode = LockMode::Write,
          .handler = Handler{expire_handler}}},
        {"INCR",
         {.arity = 2,
          .first_key = 1,
          .last_key = 1,
          .key_step = 1,
          .lock_mode = LockMode::Write,
          .handler = Handler{incr_handler}}},
        {"DEL",
         {.arity = -2,
          .first_key = 1,
          .last_key = -1,
          .key_step = 1,
          .lock_mode = LockMode::Write,
          .handler = MultiKeyHandler{del_handler}}},
        {"EXISTS",
         {.arity = -2,
          .first_key = 1,
          .last_key = -1,
          .key_step = 1,
          .lock_mode = LockMode::Read,
          .handler = MultiKeyHandler{exists_handler}}},
    };
    return table;
}

bool arity_matches(int arity, size_t n) {
    const auto signed_n = static_cast<int>(n);
    return arity >= 0 ? signed_n == arity : signed_n >= -arity;
}

namespace {

// Echoes at most this much of a client-supplied command name back in an
// error, so a huge "command name" can't turn one error reply into a huge one.
constexpr size_t kMaxEchoedNameLen = 128;

// A command name is arbitrary client bytes; it must not be able to smuggle a
// CR or LF into a RESP simple-error line and end it early.
std::string printable_name(std::string_view name) {
    std::string out(name.substr(0, kMaxEchoedNameLen));
    for (char& c : out) {
        if (c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return out;
}

}  // namespace

std::string unknown_command_error(std::string_view name) {
    return "-ERR unknown command '" + printable_name(name) + "'\r\n";
}

std::string wrong_arity_error(std::string_view name) {
    std::string lower = printable_name(name);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return "-ERR wrong number of arguments for '" + lower + "' command\r\n";
}

std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

}  // namespace mini_dragonfly::commands
