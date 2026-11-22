#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include "parse_int.hh"

namespace mini_dragonfly {

// The largest and smallest number of seconds that can be added to
// steady_clock::now() without overflowing its underlying (nanosecond,
// typically) representation. A seconds value outside this range, once
// converted to that finer resolution, wraps around to a garbage time
// point -- often one that's already in the past, making expire() with a
// huge value expire the key immediately instead of leaving it alone.
// ShardStoreImpl::expire() clamps to this range internally so it can
// never misbehave; callers validating user input may prefer to reject
// values outside it outright instead (see server.cpp's do_expire()).
inline constexpr long long kMaxExpirySeconds =
    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::max())
        .count();
inline constexpr long long kMinExpirySeconds =
    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::min())
        .count();

namespace detail {

// now + seconds, saturating instead of overflowing. Clamping `seconds`
// alone to [kMinExpirySeconds, kMaxExpirySeconds] isn't enough: those
// bounds only guarantee the *conversion* to Duration's native resolution
// doesn't overflow in isolation. now() itself has a nonzero
// time_since_epoch() (time since the clock's reference point, e.g. system
// boot) that eats into the same int64_t range, so adding a delta that
// alone fits can still push the sum past the representable maximum.
template <typename Clock>
typename Clock::time_point saturating_add_seconds(typename Clock::time_point now,
                                                  long long seconds) {
    using Duration = typename Clock::duration;
    using Rep = typename Duration::rep;

    seconds = std::clamp(seconds, kMinExpirySeconds, kMaxExpirySeconds);
    const Rep delta = std::chrono::duration_cast<Duration>(std::chrono::seconds(seconds)).count();
    const Rep now_count = now.time_since_epoch().count();

    if (delta > 0 && now_count > std::numeric_limits<Rep>::max() - delta) {
        return Clock::time_point::max();
    }
    if (delta < 0 && now_count < std::numeric_limits<Rep>::min() - delta) {
        return Clock::time_point::min();
    }
    return now + Duration(delta);
}

}  // namespace detail

// Owns one disjoint slice of the keyspace. Only ever touched by the one
// shard it lives on (via seastar::sharded<ShardStore>), so no locking.
// Pure C++ with no Seastar dependency, so it can be unit-tested directly.
//
// Templated on Clock purely so tests can substitute a fake, manually
// advanced clock (see tests/fake_clock.hh) to exercise expiry --
// including sweep_expired() below -- deterministically, instead of
// sleeping for real time to pass. Production code always uses the
// ShardStore alias at the bottom of this file; FakeClock shares
// std::chrono::steady_clock's duration/rep/period, so the bounds above
// apply unchanged regardless of which clock is in use.
template <typename Clock>
class ShardStoreImpl {
public:
    using time_point = typename Clock::time_point;

    // A key past its expiry is treated as a miss and erased on access
    // (lazy expiration). sweep_expired() below additionally reclaims
    // expired keys that are never accessed again.
    std::optional<std::string> get(const std::string& key) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return std::nullopt;
        }
        if (is_expired(it->second)) {
            erase_entry(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    // A plain set always clears any previously set expiry on the key.
    void set(std::string key, std::string value) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            data_.emplace(std::move(key), Entry{std::move(value), std::nullopt, {}});
            return;
        }
        unindex_expiry(it->second);
        it->second.value = std::move(value);
        it->second.expires_at.reset();
    }

    // Sets an expiry on an existing key, seconds from now. Returns false if
    // the key doesn't exist (or was already expired). A non-positive
    // seconds value expires the key immediately. Values outside
    // [kMinExpirySeconds, kMaxExpirySeconds] are clamped rather than
    // overflowing.
    bool expire(const std::string& key, long long seconds) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        if (is_expired(it->second)) {
            erase_entry(it);
            return false;
        }
        unindex_expiry(it->second);
        const time_point new_expiry = detail::saturating_add_seconds<Clock>(Clock::now(), seconds);
        it->second.expires_at = new_expiry;
        it->second.expiry_it = expiry_index_.emplace(new_expiry, key);
        return true;
    }

    // Removes a key. Returns true if it was present (and not expired).
    bool del(const std::string& key) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        const bool was_live = !is_expired(it->second);
        erase_entry(it);
        return was_live;
    }

    // True if the key is present and not expired. Like get(), erases the
    // entry first if it's actually just expired.
    bool exists(const std::string& key) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        if (is_expired(it->second)) {
            erase_entry(it);
            return false;
        }
        return true;
    }

    struct IncrOutcome {
        enum class Status { Ok, NotInteger, Overflow } status = Status::Ok;
        long long value = 0;  // Only meaningful when status == Ok.
    };

    // Parses the key's current value as a 64-bit integer (treating a
    // missing or expired key as 0), adds 1, and stores the result back as
    // a string -- same contract as Redis's INCR. Leaves any existing
    // expiry untouched (unlike set(), this never clears it).
    IncrOutcome incr(const std::string& key) {
        auto it = data_.find(key);
        if (it != data_.end() && is_expired(it->second)) {
            erase_entry(it);
            it = data_.end();
        }

        long long current = 0;
        if (it != data_.end() && !parse_integer(it->second.value, current)) {
            return {IncrOutcome::Status::NotInteger};
        }
        if (current == std::numeric_limits<long long>::max()) {
            return {IncrOutcome::Status::Overflow};
        }

        const long long next = current + 1;
        if (it != data_.end()) {
            it->second.value = std::to_string(next);
        } else {
            data_.emplace(key, Entry{std::to_string(next), std::nullopt, {}});
        }
        return {IncrOutcome::Status::Ok, next};
    }

    // Actively removes every currently-expired key, rather than waiting
    // for each one to be touched again -- intended to be called
    // periodically from a background task (see ExpirySweeper), not on
    // every request. expiry_index_ is kept ordered by expires_at, so this
    // only ever visits keys that have actually expired (plus one
    // upper_bound() lookup), rather than scanning the whole keyspace --
    // cost is O(k log n) in the number of keys removed, not the number of
    // keys that exist. Returns how many keys were removed.
    size_t sweep_expired() {
        size_t removed = 0;
        const auto end = expiry_index_.upper_bound(Clock::now());
        for (auto it = expiry_index_.begin(); it != end;) {
            data_.erase(it->second);
            it = expiry_index_.erase(it);
            ++removed;
        }
        return removed;
    }

private:
    using ExpiryIndex = std::multimap<time_point, std::string>;

    struct Entry {
        std::string value;
        std::optional<time_point> expires_at;
        // Valid (and meaningful) only when expires_at has a value -- points
        // at this key's entry in expiry_index_, so clearing/replacing the
        // expiry can erase it in O(log n) instead of searching for it.
        typename ExpiryIndex::iterator expiry_it;
    };

    static bool is_expired(const Entry& entry) {
        return entry.expires_at.has_value() && Clock::now() >= *entry.expires_at;
    }

    // Removes entry's expiry_index_ entry, if it has one. Leaves data_
    // untouched -- callers either overwrite the expiry right after (set(),
    // expire()) or are about to erase the whole entry (erase_entry()).
    void unindex_expiry(Entry& entry) {
        if (entry.expires_at.has_value()) {
            expiry_index_.erase(entry.expiry_it);
        }
    }

    // Removes `it` from both data_ and expiry_index_ (if it had an entry
    // there). Used wherever a key turns out to be expired and needs
    // reclaiming, or is being deleted outright.
    void erase_entry(typename std::unordered_map<std::string, Entry>::iterator it) {
        unindex_expiry(it->second);
        data_.erase(it);
    }

    std::unordered_map<std::string, Entry> data_;
    // Ordered by expires_at so sweep_expired() can find exactly the
    // expired keys without scanning data_. Keys with no expiry set are
    // never indexed here at all.
    ExpiryIndex expiry_index_;
};

using ShardStore = ShardStoreImpl<std::chrono::steady_clock>;

}  // namespace mini_dragonfly
