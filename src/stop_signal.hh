#pragma once

#include <seastar/core/condition-variable.hh>
#include <seastar/core/signal.hh>

#include <csignal>

namespace mini_dragonfly {

// Lets the reactor's main thread block on SIGINT/SIGTERM for a clean shutdown.
// Based on Seastar's own apps/lib/stop_signal.hh example helper.
class stop_signal {
public:
    stop_signal() {
        seastar::handle_signal(SIGINT, [this] { signaled(); });
        seastar::handle_signal(SIGTERM, [this] { signaled(); });
    }

    ~stop_signal() {
        // There's no way to unregister a handler yet, so register a no-op instead.
        seastar::handle_signal(SIGINT, [] {});
        seastar::handle_signal(SIGTERM, [] {});
    }

    seastar::future<> wait() {
        return cond_.wait([this] { return caught_; });
    }

private:
    void signaled() {
        if (caught_) {
            return;
        }
        caught_ = true;
        cond_.broadcast();
    }

    bool caught_ = false;
    seastar::condition_variable cond_;
};

}  // namespace mini_dragonfly
