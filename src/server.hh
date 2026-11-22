#pragma once

#include "store.hh"

#include <seastar/core/future.hh>
#include <seastar/core/gate.hh>
#include <seastar/core/shared_ptr.hh>
#include <seastar/core/sharded.hh>
#include <seastar/net/api.hh>

#include <cstdint>
#include <unordered_set>

namespace mini_dragonfly {

namespace detail {
class Connection;
}  // namespace detail

class TcpServer {
public:
    TcpServer(uint16_t port, seastar::sharded<ShardStore>& store);
    // Declared out-of-line since it must be defined where Connection (only
    // forward-declared here) is a complete type -- see server.cpp.
    ~TcpServer();

    void start();
    seastar::future<> stop();

private:
    seastar::future<> accept_loop();

    uint16_t port_;
    seastar::sharded<ShardStore>& store_;
    seastar::server_socket listener_;
    // Set only once seastar::listen() in start() actually succeeds --
    // listener_ stays default-constructed (no underlying impl) if it
    // throws, e.g. because another process already has the port. stop()
    // must not touch listener_ unless this is true: a default-constructed
    // server_socket's abort_accept() dereferences a null impl pointer.
    bool listening_ = false;
    // Set just before abort_accept() in stop(), so accept_loop() can tell
    // a deliberate shutdown cancellation apart from a transient accept()
    // failure (e.g. EMFILE) that should be logged and retried instead of
    // killing the loop for good.
    bool stopping_ = false;
    seastar::future<> accept_loop_done_ = seastar::make_ready_future<>();
    // Tracks every connection currently being served, so stop() can
    // forcibly unblock and wait for them instead of leaving them to run
    // past shutdown against a store that's about to be torn down.
    std::unordered_set<seastar::lw_shared_ptr<detail::Connection>> connections_;
    seastar::gate connections_gate_;
};

}  // namespace mini_dragonfly
