#include "store.hh"

#include <algorithm>
#include <limits>
#include <utility>

namespace mini_dragonfly {

namespace {

// now + seconds, saturating instead of overflowing. Clamping `seconds`
// alone to [kMinExpirySeconds, kMaxExpirySeconds] isn't enough: those
// bounds only guarantee the *conversion* to Duration's native resolution
// doesn't overflow in isolation. now() itself has a nonzero
// time_since_epoch() (time since the clock's reference point, e.g. system
// boot) that eats into the same int64_t range, so adding a delta that
// alone fits can still push the sum past the representable maximum.
std::chrono::steady_clock::time_point saturating_add_seconds(
    std::chrono::steady_clock::time_point now, long long seconds) {
    using Clock = std::chrono::steady_clock;
    using Duration = Clock::duration;
    using Rep = Duration::rep;

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

}  // namespace

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
    it->second.expires_at = saturating_add_seconds(std::chrono::steady_clock::now(), seconds);
    return true;
}

}  // namespace mini_dragonfly
