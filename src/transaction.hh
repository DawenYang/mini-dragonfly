#pragma once

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <string>
#include <vector>

#include "commands.hh"
#include "store.hh"
#include "tx_shard.hh"

namespace mini_dragonfly {

// Runs one command (already arity-checked against spec) as a transaction
// and resolves with its complete RESP reply. The calling shard acts as the
// coordinator: it never touches another shard's data itself, only sends
// messages to the owners and waits.
//
// Commands without keys run directly on the calling shard. Commands whose
// keys all live on one shard take a single message. Anything spanning shards
// is scheduled everywhere first and then executed -- see TxShard.
//
// stores and tx_shards must outlive the returned future.
seastar::future<std::string> execute_command(seastar::sharded<ShardStore>& stores,
                                             seastar::sharded<TxShard>& tx_shards,
                                             commands::CommandSpec spec,
                                             std::vector<std::string> args);

}  // namespace mini_dragonfly
