#include "expiry_sweeper.hh"

#include <cstddef>
#include <exception>
#include <seastar/core/do_with.hh>
#include <seastar/core/loop.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/later.hh>
#include <seastar/util/log.hh>
#include <utility>

namespace mini_dragonfly {

namespace {
seastar::logger applog("expiry_sweeper");

// Sweeps one shard's store in bounded batches, yielding to the reactor
// between them, so a burst of keys all expiring together costs many short
// tasks rather than one long stall (see ShardStore::sweep_expired()).
seastar::future<> sweep_shard(ShardStore& s) {
    return seastar::do_with(size_t{0}, [&s](size_t& total) {
        return seastar::repeat([&s, &total] {
                   const auto batch = s.sweep_expired();
                   total += batch.removed;
                   if (!batch.more) {
                       return seastar::make_ready_future<seastar::stop_iteration>(
                           seastar::stop_iteration::yes);
                   }
                   return seastar::yield().then([] { return seastar::stop_iteration::no; });
               })
            .then([&total] {
                if (total > 0) {
                    applog.info("expiry sweep removed {} key(s) on shard {}", total,
                                seastar::this_shard_id());
                }
            });
    });
}
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
                                     .then([this] { return store_.invoke_on_all(&sweep_shard); })
                                     .handle_exception_type([](const seastar::sleep_aborted&) {
                                         // Expected on stop() -- do_until's predicate is now
                                         // true and ends the loop right after this.
                                     })
                                     .handle_exception([](std::exception_ptr e) {
                                         // Anything else (e.g. one shard's sweep failing)
                                         // would otherwise end do_until's loop for good and
                                         // hand stop() an exceptional future -- which
                                         // deferred_stop's noexcept destructor (see
                                         // main.cpp) would then terminate() the whole
                                         // process on. Log it and keep the periodic sweep
                                         // running instead.
                                         applog.warn(
                                             "expiry sweep failed, will retry next tick: {}", e);
                                     });
                             });
}

}  // namespace mini_dragonfly
