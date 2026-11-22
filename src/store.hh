#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

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

// Parses a stored value as an integer for INCR. Rejects empty strings and
// trailing junk, same contract as server.cpp's parse_integer_arg (which
// parses a client's incoming argument rather than a value already in the
// store, so it's duplicated here rather than shared).
inline bool parse_stored_integer(const std::string& s, long long& out) {
    if (s.empty()) {
        return false;
    }
    const auto* begin = s.data();
    const auto* end = s.data() + s.size();
    const auto parsed = std::from_chars(begin, end, out);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

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
            data_.erase(it);
            return std::nullopt;
        }
        return it->second.value;
    }

    // A plain set always clears any previously set expiry on the key.
    void set(std::string key, std::string value) {
        data_[std::move(key)] = Entry{std::move(value), std::nullopt};
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
            data_.erase(it);
            return false;
        }
        it->second.expires_at = detail::saturating_add_seconds<Clock>(Clock::now(), seconds);
        return true;
    }

    // Removes a key. Returns true if it was present (and not expired).
    bool del(const std::string& key) {
        auto it = data_.find(key);
        if (it == data_.end()) {
            return false;
        }
        const bool was_live = !is_expired(it->second);
        data_.erase(it);
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
            data_.erase(it);
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
            data_.erase(it);
            it = data_.end();
        }

        long long current = 0;
        if (it != data_.end() && !detail::parse_stored_integer(it->second.value, current)) {
            return {IncrOutcome::Status::NotInteger};
        }
        if (current == std::numeric_limits<long long>::max()) {
            return {IncrOutcome::Status::Overflow};
        }

        const long long next = current + 1;
        if (it != data_.end()) {
            it->second.value = std::to_string(next);
        } else {
            data_[key] = Entry{std::to_string(next), std::nullopt};
        }
        return {IncrOutcome::Status::Ok, next};
    }

    // Actively removes every currently-expired key, rather than waiting
    // for each one to be touched again -- intended to be called
    // periodically from a background task (see ExpirySweeper), not on
    // every request. A full scan is simple and avoids the hazard of a
    // cursor persisted across calls: inserting into data_ between sweeps
    // can rehash the table, which would invalidate a saved iterator.
    // Returns how many keys were removed.
    size_t sweep_expired() {
        size_t removed = 0;
        for (auto it = data_.begin(); it != data_.end();) {
            if (is_expired(it->second)) {
                it = data_.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

private:
    struct Entry {
        std::string value;
        std::optional<time_point> expires_at;
    };

    static bool is_expired(const Entry& entry) {
        return entry.expires_at.has_value() && Clock::now() >= *entry.expires_at;
    }

    std::unordered_map<std::string, Entry> data_;
};

using ShardStore = ShardStoreImpl<std::chrono::steady_clock>;

}  // namespace mini_dragonfly
