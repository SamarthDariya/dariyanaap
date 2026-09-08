#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "stats/buckets.hpp"

using namespace dariyanaap;

// Chunk 1.1 ships one assertion only: that the layout constants agree with the
// arithmetic that derives them. The real exhaustive tests are chunk 1.2.
TEST_CASE("the layout is self-consistent at its edges") {
    static_assert(buckets::kSubBuckets == 128);
    static_assert(buckets::kCount == 3808);

    CHECK(buckets::index_of(0) == 0);
    CHECK(buckets::index_of(127) == 127);
    CHECK(buckets::index_of(128) == 128);
    CHECK(buckets::index_of(buckets::kMaxValue) == buckets::kCount - 1);

    CHECK(buckets::slot_low(0) == 0);
    CHECK(buckets::slot_high(127) == 127);
    CHECK(buckets::slot_low(buckets::kCount - 1) <= buckets::kMaxValue);
    CHECK(buckets::slot_high(buckets::kCount - 1) >= buckets::kMaxValue);
}
