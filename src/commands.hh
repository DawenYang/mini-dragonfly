#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "store.hh"

namespace mini_dragonfly::commands {

// What the transaction layer should acquire for this command's key(s)
// before running it. Not enforced anywhere yet (there's no transaction
// layer yet to enforce it) -- declared per-command here so that layer can
// be added later without re-deriving this information for every command
// again from scratch.
enum class LockMode { None, Read, Write };

// Runs once, given the full parsed command (args[0] is the command name
// itself) and the ShardStore that owns this command's one key -- or, for
// a command with no keys at all (CommandSpec::first_key == 0), whichever
// shard happens to be running it, since the handler is then free to
// ignore the ShardStore entirely. Returns the complete, final RESP reply.
// args is mutable so a handler storing part of it (e.g. SET) can move
// out of it instead of copying; it's only ever used for this one call.
using Handler = std::string (*)(ShardStore&, std::vector<std::string>& args);

// Runs once per key for a command whose keys are a flat, step-1 list with
// no other arguments (DEL, EXISTS, ...). The executor sums the per-key
// results into a single RESP integer reply -- see CommandSpec::handler.
using MultiKeyHandler = bool (*)(ShardStore&, const std::string& key);

struct CommandSpec {
    // Redis's own convention: a positive value is the exact number of
    // arguments (including the command name); a negative value is a
    // minimum, i.e. -3 means "at least 3". This alone can't express an
    // upper bound distinct from "exact" -- a command needing one (e.g.
    // PING, capped at 2) checks it inside its own handler instead, same
    // as real Redis does.
    int arity;

    // 1-based index of this command's first and last key argument, and
    // the spacing between consecutive keys. first_key == 0 means the
    // command has no keys at all (PING, CONFIG, ...), in which case
    // last_key and key_step are unused. last_key == -1 means "through the
    // end of args" (DEL, EXISTS, ...).
    int first_key;
    int last_key;
    int key_step;

    LockMode lock_mode;

    // Exactly one of these is active: Handler for a command with at most
    // one key (first_key == 0, or first_key == last_key), MultiKeyHandler
    // for one with a variable-length list of them (last_key == -1). The
    // executor (Connection::execute() in server.cpp) picks its path from
    // which alternative is engaged, and uses first_key/last_key/key_step
    // to find the keys -- so the two must agree, which commands_test.cpp
    // checks for every table entry.
    std::variant<Handler, MultiKeyHandler> handler;
};

const std::unordered_map<std::string, CommandSpec>& table();

// True if n (a full command's argument count, including its name)
// satisfies arity under the Redis convention described on
// CommandSpec::arity above.
bool arity_matches(int arity, size_t n);

std::string to_upper(std::string s);

// Complete RESP error replies, matching what Redis clients expect. Both
// echo a client-supplied command name, so they strip CR/LF from it and cap
// its length.
std::string unknown_command_error(std::string_view name);
std::string wrong_arity_error(std::string_view name);

}  // namespace mini_dragonfly::commands
