#include "expiry_sweeper.hh"

#include <cstddef>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/log.hh>
#include <utility>

namespace mini_dragonfly {

namespace {
seastar::logger applog("expiry_sweeper");
}  // namespace

ExpirySweeper::ExpirySweeper(seastar::sharded<ShardStore>& store,
                             std::chrono::milliseconds interval)
    : store_(store), interval_(interval) {}

void ExpirySweeper::start() {
    loop_done_ = sweep_loop();
}

seastar::future<> ExpirySweeper::stop() {
    abort_source_.request_abort();
    return std::move(loop_done_);
}

seastar::future<> ExpirySweeper::sweep_loop() {
    return seastar::do_until([this] { return abort_source_.abort_requested(); },
                             [this] {
                                 return seastar::sleep_abortable(interval_, abort_source_)
                                     .then([this] {
                                         return store_.invoke_on_all([](ShardStore& s) {
                                             const size_t removed = s.sweep_expired();
                                             if (removed > 0) {
                                                 applog.info(
                                                     "expiry sweep removed {} key(s) on shard {}",
                                                     removed, seastar::this_shard_id());
                                             }
                                         });
                                     })
                                     .handle_exception_type([](const seastar::sleep_aborted&) {
                                         // Expected on stop() -- do_until's predicate is now
                                         // true and ends the loop right after this.
                                     });
                             });
}

}  // namespace mini_dragonfly
