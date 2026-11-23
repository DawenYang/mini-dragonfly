#include "store.hh"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <limits>
#include <type_traits>
#include <utility>

#include "fake_clock.hh"

using mini_dragonfly::ShardStore;
using mini_dragonfly::ShardStoreImpl;
using FakeStore = ShardStoreImpl<test_support::FakeClock>;

TEST_CASE("get on a missing key returns nullopt", "[store]") {
    ShardStore store;
    REQUIRE_FALSE(store.get("missing").has_value());
}

TEST_CASE("set then get round-trips the value", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    const auto value = store.get("foo");
    REQUIRE(value.has_value());
    REQUIRE(*value == "bar");
}

TEST_CASE("set overwrites a previous value", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    store.set("foo", "baz");
    REQUIRE(*store.get("foo") == "baz");
}

TEST_CASE("expire on a missing key returns false", "[store]") {
    ShardStore store;
    REQUIRE_FALSE(store.expire("missing", 10));
}

TEST_CASE("expire on an existing key returns true and leaves it readable before it elapses",
          "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 10));
    REQUIRE(*store.get("foo") == "bar");
}

TEST_CASE("a zero-second expiry makes the key an immediate miss", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 0));
    REQUIRE_FALSE(store.get("foo").has_value());
}

TEST_CASE("a negative expiry also makes the key an immediate miss", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", -5));
    REQUIRE_FALSE(store.get("foo").has_value());
}

TEST_CASE("expire on an already-expired key returns false", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 0));
    // foo is now expired but not yet touched by get(), so this checks that
    // expire() itself also treats it as gone rather than refreshing it.
    REQUIRE_FALSE(store.expire("foo", 10));
}

TEST_CASE("set clears any previously set expiry", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 0));  // Expires foo immediately.
    store.set("foo", "baz");          // A fresh SET should un-expire it.
    REQUIRE(*store.get("foo") == "baz");
}

TEST_CASE("an absurdly large expiry is clamped instead of overflowing into the past", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    // Large enough that seconds * 1e9 (steady_clock's typical nanosecond
    // resolution) overflows int64_t and wraps to a point in the past if
    // not clamped -- this exact value previously made the key expire
    // immediately instead of ~292 years from now.
    REQUIRE(store.expire("foo", 9223372037LL));
    REQUIRE(*store.get("foo") == "bar");
}

TEST_CASE("an absurdly negative expiry is clamped rather than underflowing", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    // Still expires immediately (any clamp result here is far in the
    // past), but must not crash or produce UB computing it.
    REQUIRE(store.expire("foo", -9223372037LL));
    REQUIRE_FALSE(store.get("foo").has_value());
}

TEST_CASE("del on a missing key returns false", "[store]") {
    ShardStore store;
    REQUIRE_FALSE(store.del("missing"));
}

TEST_CASE("del on an existing key removes it and returns true", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.del("foo"));
    REQUIRE_FALSE(store.get("foo").has_value());
}

TEST_CASE("del on an already-expired key returns false", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 0));
    REQUIRE_FALSE(store.del("foo"));
}

TEST_CASE("exists on a missing key returns false", "[store]") {
    ShardStore store;
    REQUIRE_FALSE(store.exists("missing"));
}

TEST_CASE("exists on a live key returns true without removing it", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.exists("foo"));
    REQUIRE(*store.get("foo") == "bar");
}

TEST_CASE("exists on an expired key returns false", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 0));
    REQUIRE_FALSE(store.exists("foo"));
}

TEST_CASE("incr on a missing key starts it at 1", "[store]") {
    ShardStore store;
    const auto outcome = store.incr("foo");
    REQUIRE(outcome.status == ShardStore::IncrOutcome::Status::Ok);
    REQUIRE(outcome.value == 1);
    REQUIRE(*store.get("foo") == "1");
}

TEST_CASE("incr on an existing integer value adds one", "[store]") {
    ShardStore store;
    store.set("foo", "41");
    const auto outcome = store.incr("foo");
    REQUIRE(outcome.status == ShardStore::IncrOutcome::Status::Ok);
    REQUIRE(outcome.value == 42);
    REQUIRE(*store.get("foo") == "42");
}

TEST_CASE("incr on a non-integer value fails without modifying it", "[store]") {
    ShardStore store;
    store.set("foo", "bar");
    const auto outcome = store.incr("foo");
    REQUIRE(outcome.status == ShardStore::IncrOutcome::Status::NotInteger);
    REQUIRE(*store.get("foo") == "bar");
}

TEST_CASE("incr on an expired key treats it as missing, starting over at 1", "[store]") {
    ShardStore store;
    store.set("foo", "not a number at all");
    REQUIRE(store.expire("foo", 0));
    const auto outcome = store.incr("foo");
    REQUIRE(outcome.status == ShardStore::IncrOutcome::Status::Ok);
    REQUIRE(outcome.value == 1);
}

TEST_CASE("incr at LLONG_MAX reports overflow instead of wrapping", "[store]") {
    ShardStore store;
    store.set("foo", std::to_string(std::numeric_limits<long long>::max()));
    const auto outcome = store.incr("foo");
    REQUIRE(outcome.status == ShardStore::IncrOutcome::Status::Overflow);
    // The overflowing attempt must not have changed the stored value.
    REQUIRE(*store.get("foo") == std::to_string(std::numeric_limits<long long>::max()));
}

// The tests below use FakeClock (ShardStoreImpl<test_support::FakeClock>)
// instead of the real-clock ShardStore alias, so expiry can be driven by
// explicitly advancing time rather than sleeping for real time to pass.

TEST_CASE("a key's expiry is driven by the clock, not wall-clock sleeping", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 10));

    test_support::FakeClock::advance(std::chrono::seconds(5));
    REQUIRE(*store.get("foo") == "bar");  // Not yet expired.

    test_support::FakeClock::advance(std::chrono::seconds(6));  // 11s total.
    REQUIRE_FALSE(store.get("foo").has_value());                // Now expired.
}

TEST_CASE("sweep_expired actively removes an expired key that's never been touched", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 10));

    test_support::FakeClock::advance(std::chrono::seconds(11));
    // Deliberately no get()/exists()/del() call here -- sweep_expired()
    // alone must find and remove it, to prove this is active expiry
    // rather than lazy expiry piggybacking on some other access.
    REQUIRE(store.sweep_expired().removed == 1);
}

TEST_CASE("sweep_expired leaves live keys alone and reports zero removed", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 10));

    test_support::FakeClock::advance(std::chrono::seconds(5));
    REQUIRE(store.sweep_expired().removed == 0);
    REQUIRE(*store.get("foo") == "bar");
}

TEST_CASE("sweep_expired removes only the keys that have actually expired", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("short", "a");
    store.set("long", "b");
    store.set("forever", "c");  // No expiry set at all.
    REQUIRE(store.expire("short", 5));
    REQUIRE(store.expire("long", 20));

    test_support::FakeClock::advance(std::chrono::seconds(10));
    REQUIRE(store.sweep_expired().removed == 1);  // Only "short" has elapsed.

    REQUIRE_FALSE(store.get("short").has_value());
    REQUIRE(*store.get("long") == "b");
    REQUIRE(*store.get("forever") == "c");
}

TEST_CASE("sweep_expired on an empty store does nothing", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    REQUIRE(store.sweep_expired().removed == 0);
}

// sweep_expired() is backed by an index ordered by expires_at (see
// store.hh) rather than scanning every key, so it's only as cheap as it's
// supposed to be if every mutation that changes or clears a key's expiry
// keeps that index in sync. These guard specifically against a stale
// index entry surviving a re-expire, a set() clearing a TTL, or a del() --
// each would otherwise cost nothing today but silently reintroduce an
// O(n) element into what should be an O(removed) sweep, or report a wrong
// count.

TEST_CASE("re-expiring a key replaces its old index entry instead of leaving it stale", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 100));  // Would fire at t=100s if left alone.
    REQUIRE(store.expire("foo", 5));    // Replaces it: now fires at t=5s.

    test_support::FakeClock::advance(std::chrono::seconds(6));
    REQUIRE(store.sweep_expired().removed == 1);  // The t=5s entry, found and removed.

    test_support::FakeClock::advance(std::chrono::seconds(95));  // t=101s total.
    // If the old t=100s entry had survived the re-expire above, it would
    // show up here as a second (wrong) removal.
    REQUIRE(store.sweep_expired().removed == 0);
}

TEST_CASE("set() clearing a key's TTL removes its index entry, not just its visible expiry",
          "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 5));
    store.set("foo", "baz");  // Clears the expiry.

    test_support::FakeClock::advance(std::chrono::seconds(10));
    // A surviving stale index entry would make this wrongly find and
    // remove "foo", even though set() made it permanent again.
    REQUIRE(store.sweep_expired().removed == 0);
    REQUIRE(*store.get("foo") == "baz");
}

TEST_CASE("del() on a key with a TTL removes its index entry too", "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("foo", "bar");
    REQUIRE(store.expire("foo", 5));
    REQUIRE(store.del("foo"));

    test_support::FakeClock::advance(std::chrono::seconds(10));
    REQUIRE(store.sweep_expired().removed == 0);
}

TEST_CASE("sweep_expired removes at most max_keys per call and reports whether more remain",
          "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    constexpr int kKeys = 10'000;
    constexpr size_t kBatch = 1'000;
    for (int i = 0; i < kKeys; ++i) {
        const std::string key = "k" + std::to_string(i);
        store.set(key, "v");
        REQUIRE(store.expire(key, 5));
    }
    store.set("live", "v");  // No expiry -- must survive every batch.

    test_support::FakeClock::advance(std::chrono::seconds(6));  // All kKeys expire together.

    size_t total = 0;
    int batches = 0;
    FakeStore::SweepResult batch;
    do {
        batch = store.sweep_expired(kBatch);
        REQUIRE(batch.removed <= kBatch);
        total += batch.removed;
        ++batches;
    } while (batch.more);

    REQUIRE(total == kKeys);
    REQUIRE(batches == kKeys / static_cast<int>(kBatch));
    REQUIRE(*store.get("live") == "v");
}

TEST_CASE("sweep_expired reports no more remaining when the batch limit lands exactly on the end",
          "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    for (const char* key : {"a", "b", "c"}) {
        store.set(key, "v");
        REQUIRE(store.expire(key, 1));
    }
    test_support::FakeClock::advance(std::chrono::seconds(2));

    const auto batch = store.sweep_expired(3);
    REQUIRE(batch.removed == 3);
    REQUIRE_FALSE(batch.more);
}

TEST_CASE("sweep_expired treats a max_keys of 0 as 1, so a loop on `more` still progresses",
          "[store]") {
    test_support::FakeClock::reset();
    FakeStore store;
    store.set("a", "v");
    store.set("b", "v");
    REQUIRE(store.expire("a", 1));
    REQUIRE(store.expire("b", 1));
    test_support::FakeClock::advance(std::chrono::seconds(2));

    const auto batch = store.sweep_expired(0);
    REQUIRE(batch.removed == 1);
    REQUIRE(batch.more);
}

TEST_CASE("a store can't be copied but can be moved, keeping its expiry index intact", "[store]") {
    static_assert(!std::is_copy_constructible_v<FakeStore>);
    static_assert(!std::is_copy_assignable_v<FakeStore>);
    static_assert(std::is_move_constructible_v<FakeStore>);
    static_assert(std::is_move_assignable_v<FakeStore>);

    test_support::FakeClock::reset();
    FakeStore source;
    source.set("foo", "bar");
    REQUIRE(source.expire("foo", 5));

    FakeStore moved(std::move(source));
    FakeStore assigned;
    assigned = std::move(moved);

    // Entries' stored index iterators must still be valid in the new owner:
    // re-expiring erases through one, and sweeping walks the index itself.
    REQUIRE(assigned.expire("foo", 10));
    test_support::FakeClock::advance(std::chrono::seconds(11));
    REQUIRE(assigned.sweep_expired().removed == 1);
}
