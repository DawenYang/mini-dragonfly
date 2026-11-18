#include "store.hh"

#include <seastar/core/smp.hh>

#include <functional>
#include <utility>

namespace mini_dragonfly {

std::optional<std::string> ShardStore::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void ShardStore::set(std::string key, std::string value) { data_[std::move(key)] = std::move(value); }

unsigned owner_shard(const std::string& key) {
    return static_cast<unsigned>(std::hash<std::string>{}(key) % seastar::smp::count);
}

}  // namespace mini_dragonfly
