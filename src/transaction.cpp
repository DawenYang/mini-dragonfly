#include "transaction.hh"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <seastar/core/coroutine.hh>
#include <seastar/core/shard_id.hh>
#include <seastar/core/smp.hh>
#include <seastar/core/when_all.hh>
#include <seastar/util/log.hh>
#include <type_traits>
#include <utility>
#include <variant>

namespace mini_dragonfly {

namespace {

seastar::logger txlog("transaction");

// Shared by all shards. Relaxed is enough: only the uniqueness and the
// single modification order matter, and an id seen by a shard (via a
// cross-shard message) is smaller than any that shard allocates afterwards.
std::atomic<TxId> next_txid{1};

TxId new_txid() {
    return next_txid.fetch_add(1, std::memory_order_relaxed);
}

// The part of a transaction that belongs to one shard.
struct ShardPlan {
    unsigned shard;
    std::vector<size_t> key_indices;  // Positions of its keys in args.
    std::vector<KeyFingerprint> fingerprints;
};

// Groups the command's keys by owning shard. Must run before any handler,
// since handlers may move the keys out of args.
std::vector<ShardPlan> plan_shards(const commands::CommandSpec& spec,
                                   const std::vector<std::string>& args) {
    std::vector<ShardPlan> by_shard(seastar::smp::count);
    for (const size_t i : commands::key_indices(spec, args.size())) {
        const KeyFingerprint fingerprint = std::hash<std::string>{}(args[i]);
        ShardPlan& plan = by_shard[fingerprint % seastar::smp::count];
        plan.key_indices.push_back(i);
        plan.fingerprints.push_back(fingerprint);
    }
    std::vector<ShardPlan> plans;
    for (unsigned shard = 0; shard < by_shard.size(); ++shard) {
        if (!by_shard[shard].key_indices.empty()) {
            by_shard[shard].shard = shard;
            plans.push_back(std::move(by_shard[shard]));
        }
    }
    return plans;
}

// Arms an already scheduled transaction on this shard; resolves with func's
// result once the hop has run.
//
// noexcept: if building the promise or the hop runs out of memory, the entry
// would stay unarmed and block its queue for good.
template <typename Func>
seastar::future<std::invoke_result_t<Func&>> arm_hop(TxShard& tx, TxId id, Func func) noexcept {
    seastar::promise<std::invoke_result_t<Func&>> done;
    auto result = done.get_future();
    tx.arm(id, [done = std::move(done), func = std::move(func)]() mutable noexcept {
        try {
            done.set_value(func());
        } catch (...) {
            done.set_exception(std::current_exception());
        }
    });
    return result;
}

// A whole transaction that lives on this one shard.
template <typename Func>
seastar::future<std::invoke_result_t<Func&>> run_on_one_shard(
    TxShard& tx, const std::vector<KeyFingerprint>& keys, LockMode mode, Func func) {
    // The common case: nobody is interested in these keys, so run right now.
    if (tx.can_run_now(keys, mode)) {
        return seastar::futurize_invoke(func);
    }
    // Contended: wait in line. The id is allocated here, so it's younger
    // than everything this shard has seen and scheduling can't fail.
    const TxId id = new_txid();
    [[maybe_unused]] const bool scheduled = tx.schedule(id, keys, mode);
    assert(scheduled);
    return arm_hop(tx, id, std::move(func));
}

// A multi-shard transaction that fails partway can't be recovered: an entry
// nobody will arm or cancel blocks its shard's queue, other shards may
// already have applied their writes, and messages already sent refer to the
// coordinator's frame. Only running out of memory gets here, so give up on
// the process rather than carry on in that state.
[[noreturn]] void abort_transaction(std::exception_ptr failure) noexcept {
    txlog.error("multi-shard transaction failed partway, aborting: {}", failure);
    std::abort();
}

// Phase 1 for a multi-shard transaction. Resolves with an id that every
// involved shard has accepted. Retries have no limit, but each uses a
// younger id. A failure may leave entries behind; the caller must treat it
// as fatal.
seastar::future<TxId> schedule_everywhere(seastar::sharded<TxShard>& tx_shards,
                                          const std::vector<ShardPlan>& plans, LockMode mode) {
    while (true) {
        const TxId id = new_txid();
        std::vector<seastar::future<bool>> acks;
        acks.reserve(plans.size());
        for (const ShardPlan& plan : plans) {
            acks.push_back(tx_shards.invoke_on(plan.shard, [&plan, id, mode](TxShard& tx) {
                return tx.schedule(id, plan.fingerprints, mode);
            }));
        }
        const std::vector<bool> accepted =
            co_await seastar::when_all_succeed(acks.begin(), acks.end());
        if (std::ranges::all_of(accepted, std::identity{})) {
            co_return id;
        }
        co_await seastar::parallel_for_each(plans, [&tx_shards, id](const ShardPlan& plan) {
            return tx_shards.invoke_on(plan.shard, [id](TxShard& tx) { tx.cancel(id); });
        });
    }
}

// Runs on a key's owning shard.
std::vector<commands::KeyResult> run_keys(ShardStore& store, commands::KeyHandler handler,
                                          std::vector<std::string>& args,
                                          const std::vector<size_t>& key_indices) {
    std::vector<commands::KeyResult> results;
    results.reserve(key_indices.size());
    for (const size_t i : key_indices) {
        results.push_back(handler(store, args, i));
    }
    return results;
}

}  // namespace

// The lambdas sent to other shards below hold references into this
// coroutine's frame (args, plans). That's safe because it stays suspended
// until every shard has finished, and each shard touches only its own keys'
// elements of args.
seastar::future<std::string> execute_command(seastar::sharded<ShardStore>& stores,
                                             seastar::sharded<TxShard>& tx_shards,
                                             commands::CommandSpec spec,
                                             std::vector<std::string> args) {
    const auto* single = std::get_if<commands::Handler>(&spec.handler);
    if (spec.first_key == 0) {
        co_return (*single)(stores.local(), args);
    }

    const std::vector<ShardPlan> plans = plan_shards(spec, args);
    const LockMode mode = spec.lock_mode;

    if (single != nullptr) {
        // One key, hence one shard.
        const ShardPlan& plan = plans.front();
        const commands::Handler handler = *single;
        co_return co_await tx_shards.invoke_on(
            plan.shard, [&stores, &args, &plan, handler, mode](TxShard& tx) {
                return run_on_one_shard(tx, plan.fingerprints, mode, [&stores, &args, handler] {
                    return handler(stores.local(), args);
                });
            });
    }

    const auto multi = std::get<commands::MultiKeyHandler>(spec.handler);
    const auto work_for = [&stores, &args, multi](const ShardPlan& plan) {
        return [&stores, &args, &plan, handler = multi.per_key] {
            return run_keys(stores.local(), handler, args, plan.key_indices);
        };
    };

    std::vector<std::vector<commands::KeyResult>> per_shard;
    if (plans.size() == 1) {
        const ShardPlan& plan = plans.front();
        per_shard.push_back(co_await tx_shards.invoke_on(
            plan.shard, [&plan, mode, work = work_for(plan)](TxShard& tx) {
                return run_on_one_shard(tx, plan.fingerprints, mode, work);
            }));
    } else {
        // Nothing may escape from here -- see abort_transaction().
        try {
            const TxId id = co_await schedule_everywhere(tx_shards, plans, mode);
            // Phase 2. Past this point the transaction can't be refused.
            std::vector<seastar::future<std::vector<commands::KeyResult>>> hops;
            hops.reserve(plans.size());
            for (const ShardPlan& plan : plans) {
                hops.push_back(tx_shards.invoke_on(
                    plan.shard,
                    [id, work = work_for(plan)](TxShard& tx) { return arm_hop(tx, id, work); }));
            }
            per_shard = co_await seastar::when_all_succeed(hops.begin(), hops.end());
        } catch (...) {
            abort_transaction(std::current_exception());
        }
    }

    // Put the results back into the order the keys were given in.
    const std::vector<size_t> indices = commands::key_indices(spec, args.size());
    std::vector<commands::KeyResult> ordered(indices.size());
    for (size_t p = 0; p < plans.size(); ++p) {
        for (size_t k = 0; k < plans[p].key_indices.size(); ++k) {
            const size_t position =
                (plans[p].key_indices[k] - static_cast<size_t>(spec.first_key)) /
                static_cast<size_t>(spec.key_step);
            ordered[position] = std::move(per_shard[p][k]);
        }
    }
    co_return multi.combine(std::move(ordered));
}

}  // namespace mini_dragonfly
