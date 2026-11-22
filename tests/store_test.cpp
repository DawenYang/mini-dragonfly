#include "store.hh"

#include <catch2/catch_test_macros.hpp>

using mini_dragonfly::ShardStore;

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
    store.set("foo", "baz");         // A fresh SET should un-expire it.
    REQUIRE(*store.get("foo") == "baz");
}

TEST_CASE("an absurdly large expiry is clamped instead of overflowing into the past",
          "[store]") {
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
