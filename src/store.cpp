#include "store.hh"

#include <utility>

namespace mini_dragonfly {

bool ShardStore::is_expired(const Entry& entry) {
    return entry.expires_at.has_value() && std::chrono::steady_clock::now() >= *entry.expires_at;
}

std::optional<std::string> ShardStore::get(const std::string& key) {
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

void ShardStore::set(std::string key, std::string value) {
    data_[std::move(key)] = Entry{std::move(value), std::nullopt};
}

bool ShardStore::expire(const std::string& key, long long seconds) {
    auto it = data_.find(key);
    if (it == data_.end()) {
        return false;
    }
    if (is_expired(it->second)) {
        data_.erase(it);
        return false;
    }
    it->second.expires_at = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    return true;
}

}  // namespace mini_dragonfly
