#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace mini_dragonfly {

// Owns one disjoint slice of the keyspace. Only ever touched by the one
// shard it lives on (via seastar::sharded<ShardStore>), so no locking.
class ShardStore {
public:
    std::optional<std::string> get(const std::string& key) const;
    void set(std::string key, std::string value);

private:
    std::unordered_map<std::string, std::string> data_;
};

// Which shard owns a given key.
unsigned owner_shard(const std::string& key);

}  // namespace mini_dragonfly
