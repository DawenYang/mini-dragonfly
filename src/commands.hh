#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "store.hh"
#include "tx_shard.hh"

namespace mini_dragonfly::commands {

// Runs once, given the full parsed command (args[0] is the command name
// itself) and the ShardStore that owns this command's one key -- or, for
// a command with no keys at all (CommandSpec::first_key == 0), whichever
// shard happens to be running it, since the handler is then free to
// ignore the ShardStore entirely. Returns the complete, final RESP reply.
// args is mutable so a handler storing part of it (e.g. SET) can move
// out of it instead of copying; it's only ever used for this one call.
using Handler = std::string (*)(ShardStore&, std::vector<std::string>& args);

// What one key contributed to a multi-key command's reply. Empty means
// "nothing" (a missing key for MGET or DEL).
using KeyResult = std::optional<std::string>;

// Runs once per key, on the shard owning args[key_index]. It may only touch
// that key's own arguments, args[key_index, key_index + key_step), and may
// move out of them: other shards are working on the rest of args at the
// same time.
using KeyHandler = KeyResult (*)(ShardStore&, std::vector<std::string>& args, size_t key_index);

// Turns every key's result, in the order the keys were given, into the
// complete RESP reply.
using Combiner = std::string (*)(std::vector<KeyResult> results);

// A command whose number of keys varies (DEL, MGET, MSET, ...).
struct MultiKeyHandler {
    KeyHandler per_key;
    Combiner combine;
};

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

    // How the transaction layer locks this command's keys. None if and only
    // if first_key == 0.
    LockMode lock_mode;

    // Exactly one of these is active: Handler for a command with at most
    // one key (first_key == 0, or first_key == last_key), MultiKeyHandler
    // for one with a variable-length list of them (last_key == -1). The
    // executor (execute_command() in transaction.cpp) picks its path from
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

// As above, and also rejects an incomplete trailing key group, such as an
// MSET key without a value.
bool arity_matches(const CommandSpec& spec, size_t n);

// Positions of the key arguments in a command of n arguments. n must satisfy
// arity_matches(spec, n).
std::vector<size_t> key_indices(const CommandSpec& spec, size_t n);

std::string to_upper(std::string s);

// Complete RESP error replies, matching what Redis clients expect. Both
// echo a client-supplied command name, so they strip CR/LF from it and cap
// its length.
std::string unknown_command_error(std::string_view name);
std::string wrong_arity_error(std::string_view name);

}  // namespace mini_dragonfly::commands
