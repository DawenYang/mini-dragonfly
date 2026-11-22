#pragma once

#include <chrono>
#include <seastar/core/abort_source.hh>
#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>

#include "store.hh"

namespace mini_dragonfly {

// Periodically sweeps every shard's ShardStore for expired keys, so a key
// nobody ever touches again still eventually gets reclaimed instead of
// relying solely on lazy expiration (ShardStore::get() et al. erasing it
// only when -- and if -- it's next accessed).
class ExpirySweeper {
public:
    ExpirySweeper(seastar::sharded<ShardStore>& store, std::chrono::milliseconds interval);

    void start();

    // Stops the periodic loop and waits for whichever sweep (if any) is
    // currently in flight to finish first, so shutdown can't race it
    // against the sharded<ShardStore> being torn down right after --
    // the same class of bug TcpServer::stop() guards against for live
    // connections.
    seastar::future<> stop();

private:
    seastar::future<> sweep_loop();

    seastar::sharded<ShardStore>& store_;
    std::chrono::milliseconds interval_;
    seastar::abort_source abort_source_;
    seastar::future<> loop_done_ = seastar::make_ready_future<>();
};

}  // namespace mini_dragonfly
