#pragma once

#include "store.hh"

#include <seastar/core/future.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>

#include <cstdint>

namespace mini_dragonfly {

class TcpServer {
public:
    TcpServer(uint16_t port, seastar::sharded<ShardStore>& store);

    void start();
    seastar::future<> stop();

private:
    seastar::future<> accept_loop();

    uint16_t port_;
    seastar::sharded<ShardStore>& store_;
    seastar::server_socket listener_;
    seastar::future<> accept_loop_done_ = seastar::make_ready_future<>();
};

}  // namespace mini_dragonfly
