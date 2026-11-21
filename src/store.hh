#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <unordered_map>

namespace mini_dragonfly {

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
    // seconds value expires the key immediately.
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
