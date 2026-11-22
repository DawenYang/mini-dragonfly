#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

namespace mini_dragonfly {

// The largest and smallest number of seconds that can be added to
// steady_clock::now() without overflowing its underlying (nanosecond,
// typically) representation. A seconds value outside this range, once
// converted to that finer resolution, wraps around to a garbage time
// point -- often one that's already in the past, making expire() with a
// huge value expire the key immediately instead of leaving it alone.
// ShardStore::expire() clamps to this range internally so it can never
// misbehave; callers validating user input may prefer to reject values
// outside it outright instead (see server.cpp's do_expire()).
inline constexpr long long kMaxExpirySeconds =
    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::max())
        .count();
inline constexpr long long kMinExpirySeconds =
    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::duration::min())
        .count();

// Owns one disjoint slice of the keyspace. Only ever touched by the one
// shard it lives on (via seastar::sharded<ShardStore>), so no locking.
// Pure C++ with no Seastar dependency, so it can be unit-tested directly.
class ShardStore {
public:
    // A key past its expiry is treated as a miss and erased on access
    // (lazy expiration) -- nothing proactively sweeps expired keys yet.
    std::optional<std::string> get(const std::string& key);

    // A plain set always clears any previously set expiry on the key.
    void set(std::string key, std::string value);

    // Sets an expiry on an existing key, seconds from now. Returns false if
    // the key doesn't exist (or was already expired). A non-positive
    // seconds value expires the key immediately. Values outside
    // [kMinExpirySeconds, kMaxExpirySeconds] are clamped rather than
    // overflowing.
    bool expire(const std::string& key, long long seconds);

private:
    struct Entry {
        std::string value;
        std::optional<std::chrono::steady_clock::time_point> expires_at;
    };

    static bool is_expired(const Entry& entry);

    std::unordered_map<std::string, Entry> data_;
};

}  // namespace mini_dragonfly
