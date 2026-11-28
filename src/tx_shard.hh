#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini_dragonfly {

// Global order of transactions. 0 is never handed out.
using TxId = std::uint64_t;

// A hash of a key. Two keys colliding only makes them look contended, which
// costs some concurrency but never correctness.
using KeyFingerprint = std::uint64_t;

// What a command needs on its key(s): Read is shared, Write is exclusive.
// None is for commands without keys, which never reach this class.
enum class LockMode { None, Read, Write };

// One shard's half of a lightweight (VLL-style) transaction layer, after
// https://github.com/dragonflydb/dragonfly/blob/main/docs/transaction.md.
//
// A transaction goes through two phases:
//   1. schedule() on every shard it touches: record "intent locks" on its
//      keys and take a place in this shard's queue, ordered by TxId. Intent
//      locks never block; they only count who is interested in a key. If any
//      shard refuses, the caller cancel()s all of them and retries with a
//      new, larger TxId.
//   2. arm() on every shard with the work to do (the "hop"). It runs at once
//      if nothing conflicted when it was scheduled ("out of order"), and
//      otherwise when it reaches the head of the queue.
//
// The invariant everything rests on: any two conflicting transactions run in
// TxId order, on every shard they share. That makes the result serializable,
// and it rules out deadlock, since the oldest transaction is never waiting
// for another one.
//
// A hop is synchronous: it can't suspend, so it is atomic on its shard.
//
// Not thread-safe and Seastar-free: each instance belongs to one shard.
class TxShard {
public:
    // The work of one transaction on this shard.
    //
    // A hop must never call schedule(), arm() or cancel() on the TxShard
    // running it, directly or indirectly. It runs while its own entry is
    // still queued and armed, so a nested arm() or cancel() would reach
    // drain(), run that entry a second time and erase it from under the
    // outer run(). Anything that needs to react to a hop must do it later,
    // e.g. from a future's continuation. Debug builds assert this.
    using Hop = std::move_only_function<void() noexcept>;

    // True if no scheduled transaction conflicts with this access. A
    // transaction confined to this one shard may then simply run now: no
    // TxId, no queue entry, no locks, because nothing can interleave with a
    // synchronous callback.
    bool can_run_now(const std::vector<KeyFingerprint>& keys, LockMode mode) const {
        return std::ranges::none_of(keys, [&](KeyFingerprint key) {
            const auto it = locks_.find(key);
            if (it == locks_.end()) {
                return false;
            }
            return it->second.exclusive > 0 || (mode == LockMode::Write && it->second.shared > 0);
        });
    }

    // Returns false, changing nothing, if id arrived too late to be ordered
    // correctly:
    //   - a younger transaction has already run here. It may have touched
    //     these keys, and we can no longer get in front of it.
    //   - we conflict with someone, and a younger transaction is already
    //     queued. It may conflict with us and have been cleared to run out
    //     of order, so we can't slot in ahead of it.
    // Both checks are conservative: they don't ask whether the younger one
    // really shares a key.
    //
    // noexcept because it can't back out of a partial update: running out of
    // memory while counting intents would leave some counted with no queue
    // entry to release them, so the process terminates instead.
    bool schedule(TxId id, std::vector<KeyFingerprint> keys, LockMode mode) noexcept {
        assert(!in_hop_);
        assert(mode != LockMode::None);
        if (id < committed_) {
            return false;
        }
        // Checked before counting our own intents, so a transaction naming
        // one key twice doesn't conflict with itself.
        const bool granted = can_run_now(keys, mode);
        if (!granted && !queue_.empty() && id < queue_.rbegin()->first) {
            return false;
        }
        for (const KeyFingerprint key : keys) {
            IntentLock& lock = locks_[key];
            ++(mode == LockMode::Write ? lock.exclusive : lock.shared);
        }
        queue_.emplace(id, Entry{std::move(keys), mode, granted, Hop{}});
        return true;
    }

    // Supplies the work of a successfully scheduled transaction. It runs
    // either before this returns or during a later arm()/cancel(), and its
    // locks are released right after.
    void arm(TxId id, Hop hop) noexcept {
        assert(!in_hop_);
        const auto it = queue_.find(id);
        assert(it != queue_.end() && !it->second.hop);
        it->second.hop = std::move(hop);
        if (it->second.granted) {
            run(it);
        }
        drain();
    }

    // Withdraws a transaction that was scheduled but not armed. Does nothing
    // if id isn't here, so a caller can cancel on every shard without
    // tracking which ones accepted.
    void cancel(TxId id) noexcept {
        assert(!in_hop_);
        const auto it = queue_.find(id);
        if (it == queue_.end()) {
            return;
        }
        assert(!it->second.hop);
        release(it->second);
        queue_.erase(it);
        drain();
    }

    std::size_t queue_size() const { return queue_.size(); }
    std::size_t locked_key_count() const { return locks_.size(); }

private:
    struct IntentLock {
        std::uint32_t shared = 0;
        std::uint32_t exclusive = 0;
    };

    struct Entry {
        std::vector<KeyFingerprint> keys;
        LockMode mode;
        // Nothing conflicted at scheduling time, so this one needn't wait
        // for its turn. Later conflicting arrivals see its intents and queue
        // behind it.
        bool granted;
        Hop hop;  // Empty until armed.
    };

    using Queue = std::map<TxId, Entry>;

    void run(Queue::iterator it) {
        committed_ = std::max(committed_, it->first);
        in_hop_ = true;
        it->second.hop();
        in_hop_ = false;
        release(it->second);
        queue_.erase(it);
    }

    // Runs from the head for as long as the head is ready. An unarmed head
    // holds everyone ungranted behind it, but only until its coordinator,
    // which never waits on another transaction while scheduling, arms or
    // cancels it.
    void drain() {
        while (!queue_.empty() && queue_.begin()->second.hop) {
            run(queue_.begin());
        }
    }

    void release(const Entry& entry) {
        for (const KeyFingerprint key : entry.keys) {
            const auto it = locks_.find(key);
            assert(it != locks_.end());
            --(entry.mode == LockMode::Write ? it->second.exclusive : it->second.shared);
            if (it->second.shared == 0 && it->second.exclusive == 0) {
                locks_.erase(it);
            }
        }
    }

    std::unordered_map<KeyFingerprint, IntentLock> locks_;
    Queue queue_;
    // The largest TxId that has run here.
    TxId committed_ = 0;
    // True while a hop is running; see Hop.
    bool in_hop_ = false;
};

}  // namespace mini_dragonfly
