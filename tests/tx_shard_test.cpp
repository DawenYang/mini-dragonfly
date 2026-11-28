#include "tx_shard.hh"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <functional>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

using mini_dragonfly::KeyFingerprint;
using mini_dragonfly::LockMode;
using mini_dragonfly::TxId;
using mini_dragonfly::TxShard;

namespace {

TxShard::Hop record(std::vector<TxId>& log, TxId id) {
    return [&log, id]() noexcept { log.push_back(id); };
}

}  // namespace

TEST_CASE("readers share a key, a writer excludes everyone", "[tx]") {
    TxShard tx;
    REQUIRE(tx.can_run_now({1}, LockMode::Write));

    REQUIRE(tx.schedule(1, {1}, LockMode::Read));
    REQUIRE(tx.can_run_now({1}, LockMode::Read));
    REQUIRE_FALSE(tx.can_run_now({1}, LockMode::Write));
    REQUIRE(tx.can_run_now({2}, LockMode::Write));

    REQUIRE(tx.schedule(2, {2}, LockMode::Write));
    REQUIRE_FALSE(tx.can_run_now({2}, LockMode::Read));
    REQUIRE_FALSE(tx.can_run_now({3, 2}, LockMode::Read));
}

TEST_CASE("a conflicting transaction waits for the older one, whatever the arm order", "[tx]") {
    TxShard tx;
    std::vector<TxId> log;
    REQUIRE(tx.schedule(1, {7}, LockMode::Write));
    REQUIRE(tx.schedule(2, {7}, LockMode::Write));

    tx.arm(2, record(log, 2));
    REQUIRE(log.empty());
    tx.arm(1, record(log, 1));
    REQUIRE(log == std::vector<TxId>{1, 2});
    REQUIRE(tx.queue_size() == 0);
    REQUIRE(tx.locked_key_count() == 0);
}

TEST_CASE("an uncontended transaction runs out of order", "[tx]") {
    TxShard tx;
    std::vector<TxId> log;
    REQUIRE(tx.schedule(1, {1}, LockMode::Write));
    REQUIRE(tx.schedule(2, {2}, LockMode::Write));

    tx.arm(2, record(log, 2));
    REQUIRE(log == std::vector<TxId>{2});
    REQUIRE(tx.queue_size() == 1);
}

TEST_CASE("a late conflicting arrival is refused", "[tx]") {
    TxShard tx;
    REQUIRE(tx.schedule(5, {1}, LockMode::Write));
    REQUIRE_FALSE(tx.schedule(4, {1}, LockMode::Write));
    REQUIRE(tx.queue_size() == 1);
    // Nothing to be reordered against, so a late one is fine.
    REQUIRE(tx.schedule(3, {2}, LockMode::Write));
}

TEST_CASE("an arrival older than something already run is refused", "[tx]") {
    TxShard tx;
    std::vector<TxId> log;
    REQUIRE(tx.schedule(5, {1}, LockMode::Write));
    tx.arm(5, record(log, 5));

    REQUIRE_FALSE(tx.schedule(4, {1}, LockMode::Write));
    REQUIRE(tx.schedule(6, {1}, LockMode::Write));
}

TEST_CASE("cancel releases locks and unblocks the queue", "[tx]") {
    TxShard tx;
    std::vector<TxId> log;
    REQUIRE(tx.schedule(1, {1}, LockMode::Write));
    REQUIRE(tx.schedule(2, {1}, LockMode::Write));
    tx.arm(2, record(log, 2));

    tx.cancel(1);
    tx.cancel(99);  // Unknown ids are ignored.
    REQUIRE(log == std::vector<TxId>{2});
    REQUIRE(tx.locked_key_count() == 0);
}

TEST_CASE("naming a key twice doesn't make a transaction conflict with itself", "[tx]") {
    TxShard tx;
    std::vector<TxId> log;
    REQUIRE(tx.schedule(1, {9}, LockMode::Read));
    REQUIRE(tx.schedule(3, {4, 4}, LockMode::Write));
    // Granted, so it needn't wait for the unarmed head.
    tx.arm(3, record(log, 3));
    REQUIRE(log == std::vector<TxId>{3});
    REQUIRE(tx.locked_key_count() == 1);
}

namespace {

// A deterministic simulation of many coordinators talking to several shards,
// delivering their messages in random order. It follows the same steps as
// transaction.cpp, without Seastar.
class Simulation {
public:
    static constexpr size_t kShards = 3;
    static constexpr size_t kKeysPerShard = 3;
    static constexpr size_t kTransactions = 24;

    explicit Simulation(unsigned seed) : rng_(seed) {
        for (size_t t = 0; t < kTransactions; ++t) {
            Tx& tx = txs_[t];
            tx.mode = pick(3) == 0 ? LockMode::Read : LockMode::Write;
            for (size_t s = 0; s < kShards; ++s) {
                if (pick(2) == 0) {
                    tx.parts.push_back({s, {key(s)}});
                }
            }
            if (tx.parts.empty()) {
                const size_t s = pick(kShards);
                tx.parts.push_back({s, {key(s), key(s)}});
            }
        }
    }

    void run() {
        size_t started = 0;
        while (started < kTransactions || !inbox_.empty()) {
            if (started < kTransactions && (inbox_.empty() || pick(4) == 0)) {
                start(started++);
                continue;
            }
            const size_t i = pick(inbox_.size());
            auto message = std::move(inbox_[i]);
            inbox_.erase(inbox_.begin() + static_cast<std::ptrdiff_t>(i));
            message();
        }
    }

    // Every transaction ran on all its shards: nothing deadlocked or starved.
    bool all_completed() const {
        return std::ranges::all_of(txs_, [](const Tx& tx) { return tx.remaining == 0; }) &&
               std::ranges::all_of(shards_, [](const TxShard& s) {
                   return s.queue_size() == 0 && s.locked_key_count() == 0;
               });
    }

    // Strict serializability: conflicting accesses, plus "finished before the
    // other started", must form an acyclic order.
    bool strictly_serializable() const {
        std::vector<std::set<size_t>> edges = realtime_edges_;
        for (const auto& log : logs_) {
            for (size_t i = 0; i < log.size(); ++i) {
                for (size_t j = i + 1; j < log.size(); ++j) {
                    if (conflict(log[i], log[j])) {
                        edges[log[i].tx].insert(log[j].tx);
                    }
                }
            }
        }
        std::vector<int> state(kTransactions, 0);
        std::function<bool(size_t)> acyclic_from = [&](size_t node) {
            if (state[node] != 0) {
                return state[node] == 2;
            }
            state[node] = 1;
            for (const size_t next : edges[node]) {
                if (!acyclic_from(next)) {
                    return false;
                }
            }
            state[node] = 2;
            return true;
        };
        for (size_t t = 0; t < kTransactions; ++t) {
            if (!acyclic_from(t)) {
                return false;
            }
        }
        return true;
    }

private:
    struct Part {
        size_t shard;
        std::vector<KeyFingerprint> keys;
    };
    struct Tx {
        LockMode mode = LockMode::Read;
        std::vector<Part> parts;
        size_t remaining = 1;  // Shards yet to run it.
        size_t acks = 0;
        bool refused = false;
    };
    struct Access {
        size_t tx;
        size_t part;
    };

    size_t pick(size_t n) { return std::uniform_int_distribution<size_t>(0, n - 1)(rng_); }
    KeyFingerprint key(size_t shard) { return shard * 100 + pick(kKeysPerShard); }

    bool conflict(const Access& a, const Access& b) const {
        if (txs_[a.tx].mode == LockMode::Read && txs_[b.tx].mode == LockMode::Read) {
            return false;
        }
        const auto& other = txs_[b.tx].parts[b.part].keys;
        return std::ranges::any_of(txs_[a.tx].parts[a.part].keys, [&](KeyFingerprint k) {
            return std::ranges::find(other, k) != other.end();
        });
    }

    void start(size_t t) {
        for (size_t earlier = 0; earlier < t; ++earlier) {
            if (txs_[earlier].remaining == 0) {
                realtime_edges_[earlier].insert(t);
            }
        }
        Tx& tx = txs_[t];
        tx.remaining = tx.parts.size();
        if (tx.parts.size() > 1) {
            schedule(t);
            return;
        }
        inbox_.emplace_back([this, t] {
            const Part& part = txs_[t].parts[0];
            TxShard& shard = shards_[part.shard];
            if (shard.can_run_now(part.keys, txs_[t].mode)) {
                executed(t, 0);
                return;
            }
            const TxId id = next_id_++;
            REQUIRE(shard.schedule(id, part.keys, txs_[t].mode));
            shard.arm(id, [this, t]() noexcept { executed(t, 0); });
        });
    }

    void schedule(size_t t) {
        const TxId id = next_id_++;
        txs_[t].acks = 0;
        txs_[t].refused = false;
        for (size_t p = 0; p < txs_[t].parts.size(); ++p) {
            inbox_.emplace_back([this, t, p, id] {
                Tx& tx = txs_[t];
                const Part& part = tx.parts[p];
                if (!shards_[part.shard].schedule(id, part.keys, tx.mode)) {
                    tx.refused = true;
                }
                if (++tx.acks == tx.parts.size()) {
                    tx.refused ? cancel(t, id) : execute(t, id);
                }
            });
        }
    }

    void cancel(size_t t, TxId id) {
        txs_[t].acks = 0;
        for (size_t p = 0; p < txs_[t].parts.size(); ++p) {
            inbox_.emplace_back([this, t, p, id] {
                shards_[txs_[t].parts[p].shard].cancel(id);
                if (++txs_[t].acks == txs_[t].parts.size()) {
                    schedule(t);
                }
            });
        }
    }

    void execute(size_t t, TxId id) {
        for (size_t p = 0; p < txs_[t].parts.size(); ++p) {
            inbox_.emplace_back([this, t, p, id] {
                shards_[txs_[t].parts[p].shard].arm(id,
                                                    [this, t, p]() noexcept { executed(t, p); });
            });
        }
    }

    void executed(size_t t, size_t p) {
        logs_[txs_[t].parts[p].shard].push_back({t, p});
        --txs_[t].remaining;
    }

    std::mt19937 rng_;
    std::vector<TxShard> shards_ = std::vector<TxShard>(kShards);
    std::vector<Tx> txs_ = std::vector<Tx>(kTransactions);
    std::vector<std::vector<Access>> logs_ = std::vector<std::vector<Access>>(kShards);
    std::vector<std::set<size_t>> realtime_edges_ = std::vector<std::set<size_t>>(kTransactions);
    std::vector<std::function<void()>> inbox_;
    TxId next_id_ = 1;
};

}  // namespace

TEST_CASE("random interleavings always complete and stay strictly serializable", "[tx]") {
    for (unsigned seed = 0; seed < 5000; ++seed) {
        Simulation sim(seed);
        sim.run();
        INFO("seed " << seed);
        REQUIRE(sim.all_completed());
        REQUIRE(sim.strictly_serializable());
    }
}
