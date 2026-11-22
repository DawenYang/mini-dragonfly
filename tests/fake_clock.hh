#pragma once

#include <chrono>

namespace test_support {

// A manually-advanced stand-in for std::chrono::steady_clock, usable as
// ShardStoreImpl<Clock>'s Clock parameter so tests can exercise TTL
// expiry -- including ShardStoreImpl::sweep_expired() -- deterministically,
// by advancing time explicitly instead of sleeping for real time to pass.
// Shares steady_clock's duration/rep/period, so store.hh's overflow
// bounds (kMinExpirySeconds/kMaxExpirySeconds) apply unchanged regardless
// of which clock a given ShardStoreImpl instantiation uses.
//
// now_ is process-wide static state, not reset between TEST_CASEs
// automatically -- call reset() at the start of each one that uses this
// clock.
class FakeClock {
public:
    using duration = std::chrono::steady_clock::duration;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FakeClock, duration>;
    static constexpr bool is_steady = true;

    static time_point now() noexcept { return now_; }
    static void advance(duration d) noexcept { now_ += d; }
    static void reset() noexcept { now_ = time_point{}; }

private:
    inline static time_point now_{};
};

}  // namespace test_support
