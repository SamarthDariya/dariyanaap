#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "stats/buckets.hpp"
#include "stats/histogram.hpp"

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

// ---------------------------------------------------------------------------
// Chunk 1.2 — the layout, exhaustively. These are the tests that *derive*
// DESIGN.md decision 4's error bound rather than quoting it.
// ---------------------------------------------------------------------------

TEST_CASE("the linear region is exact, so a nanosecond is a nanosecond") {
    for (std::int64_t v = 0; v < buckets::kSubBuckets; ++v) {
        const int i = buckets::index_of(v);
        REQUIRE(i == static_cast<int>(v));
        REQUIRE(buckets::slot_low(i) == v);
        REQUIRE(buckets::slot_high(i) == v);  // width 1: no error at all down here
    }
}

TEST_CASE("indices never go backwards and never skip a slot") {
    // A skipped slot is a value no sample can ever land in, which would make
    // percentile() interpolate across a gap that does not exist.
    int previous = buckets::index_of(0);
    for (std::int64_t v = 1; v <= 3'000'000; ++v) {
        const int i = buckets::index_of(v);
        REQUIRE(i >= previous);
        REQUIRE(i <= previous + 1);
        previous = i;
    }
}

TEST_CASE("indices stay monotone all the way to 60 seconds") {
    // Multiplicative sweep: dense enough to cross every octave boundary.
    int previous = 0;
    std::int64_t v = 1;
    while (v < buckets::kMaxValue) {
        const int i = buckets::index_of(v);
        REQUIRE(i >= previous);
        previous = i;
        v = v + v / 1000 + 1;
    }
    CHECK(buckets::index_of(buckets::kMaxValue) == buckets::kCount - 1);
}

TEST_CASE("every slot round-trips through its own bounds") {
    for (int i = 0; i < buckets::kCount; ++i) {
        const std::int64_t low = buckets::slot_low(i);
        const std::int64_t high = buckets::slot_high(i);
        REQUIRE(low <= high);
        REQUIRE(buckets::index_of(low) == i);
        REQUIRE(buckets::index_of(high) == i);
    }
}

TEST_CASE("slots are contiguous, so the layout covers the range with no holes") {
    for (int i = 0; i + 1 < buckets::kCount; ++i) {
        REQUIRE(buckets::slot_high(i) + 1 == buckets::slot_low(i + 1));
    }
}

TEST_CASE("the 0.781% bound is attained and never exceeded") {
    // Reporting a slot's high edge over-reports by at most one slot width.
    // This measures that, rather than trusting the arithmetic in the header.
    double worst = 0.0;
    std::int64_t worst_at = 0;
    std::int64_t v = buckets::kSubBuckets;
    while (v < buckets::kMaxValue) {
        const double reported = static_cast<double>(buckets::slot_high(buckets::index_of(v)));
        const double error = (reported - static_cast<double>(v)) / static_cast<double>(v);
        if (error > worst) {
            worst = error;
            worst_at = v;
        }
        REQUIRE(error >= 0.0);  // high edge: never under-reports, ever
        v = v + v / 3000 + 1;
    }

    // Never exceeded: this is the claim DESIGN.md decision 4 makes.
    CHECK(worst <= 1.0 / static_cast<double>(buckets::kSubBuckets));
    // And attained: a bound that is never approached would mean the layout is
    // wasting memory on precision it does not deliver where it matters.
    CHECK(worst > 0.99 / static_cast<double>(buckets::kSubBuckets));
    CHECK(worst_at > 0);
    MESSAGE("worst over-report " << worst * 100.0 << "% at " << worst_at << " ns");
}

// ---------------------------------------------------------------------------
// Chunk 1.3 — Histogram
// ---------------------------------------------------------------------------

// Decision 3 says the histogram must never report a mean. Asserted at compile
// time rather than trusted to a comment: if anyone adds mean(), this fails.
template <class T>
concept HasMean = requires(const T& t) { t.mean(); };
static_assert(!HasMean<Histogram>, "DESIGN.md decision 3: never report a mean");

TEST_CASE("an empty histogram reports nothing rather than zero-ish nonsense") {
    const Histogram h;
    CHECK(h.count() == 0);
    CHECK(h.overflow() == 0);
    CHECK(h.max() == Nanos(0));
    for (int i = 0; i < buckets::kCount; ++i) {
        REQUIRE(h.slot(i) == 0);
    }
}

TEST_CASE("the histogram is fixed-size and small enough to keep one per thread") {
    // 30KB per thread at 500 connections is 15MB — the reason this is an array
    // and not a map, and the reason decision 5's per-thread copies are viable.
    static_assert(sizeof(Histogram) < 32 * 1024);
    CHECK(sizeof(Histogram) == buckets::kCount * sizeof(std::uint64_t) + 24);
}

TEST_CASE("recording lands values in their own slot and counts them once") {
    Histogram h;
    h.record(Nanos(42));
    h.record(Nanos(42));
    h.record(Nanos(1000));

    CHECK(h.count() == 3);
    CHECK(h.overflow() == 0);
    CHECK(h.slot(buckets::index_of(42)) == 2);
    CHECK(h.slot(buckets::index_of(1000)) == 1);
    CHECK(h.max() == Nanos(1000));
}

TEST_CASE("max is exact, not rounded up to a slot edge") {
    Histogram h;
    h.record(Micros(1500));  // 1,500,000ns — inside a slot, not on its edge
    CHECK(h.max() == Nanos(1'500'000));
    // The slot it landed in reports a higher value; max does not use it.
    CHECK(buckets::slot_high(buckets::index_of(1'500'000)) > 1'500'000);
}

TEST_CASE("a value past 60s is counted, never clamped into the top slot") {
    Histogram h;
    h.record(Secs(90));

    CHECK(h.count() == 1);
    CHECK(h.overflow() == 1);
    // Clamping would have reported a 90-second stall as 60 seconds.
    CHECK(h.max() == Secs(90));
    CHECK(h.slot(buckets::kCount - 1) == 0);
}

TEST_CASE("the boundary at 60s is inclusive, so only past it overflows") {
    Histogram h;
    h.record(Nanos(buckets::kMaxValue));
    CHECK(h.overflow() == 0);
    CHECK(h.slot(buckets::kCount - 1) == 1);

    h.record(Nanos(buckets::kMaxValue + 1));
    CHECK(h.overflow() == 1);
    CHECK(h.count() == 2);
}

// ---------------------------------------------------------------------------
// Chunk 1.4 — percentile()
// ---------------------------------------------------------------------------

TEST_CASE("an empty histogram has no percentile, rather than a flattering zero") {
    const Histogram h;
    CHECK_FALSE(h.percentile(50.0).has_value());
    CHECK_FALSE(h.percentile(99.0).has_value());
    // A run whose every request failed has no p99. Reporting 0ns would be the
    // most flattering possible under-report.
}

TEST_CASE("one sample is every percentile") {
    Histogram h;
    h.record(Nanos(42));
    CHECK(h.percentile(0.0).value() == Nanos(42));    // linear region: exact
    CHECK(h.percentile(50.0).value() == Nanos(42));
    CHECK(h.percentile(100.0).value() == Nanos(42));
}

TEST_CASE("percentiles are monotone, or p99 could come out below p50") {
    Histogram h;
    for (std::int64_t v = 1; v <= 10'000; ++v) {
        h.record(Micros(v));
    }
    const Nanos p0 = h.percentile(0.0).value();
    const Nanos p50 = h.percentile(50.0).value();
    const Nanos p90 = h.percentile(90.0).value();
    const Nanos p99 = h.percentile(99.0).value();
    const Nanos p999 = h.percentile(99.9).value();
    const Nanos p100 = h.percentile(100.0).value();

    CHECK(p0 <= p50);
    CHECK(p50 <= p90);
    CHECK(p90 <= p99);
    CHECK(p99 <= p999);
    CHECK(p999 <= p100);
}

TEST_CASE("percentiles land within the bound of the exact answer") {
    // 10,000 samples, one per microsecond from 1us to 10,000us, so the exact
    // p-th percentile is p*100 microseconds and can be checked directly.
    Histogram h;
    for (std::int64_t v = 1; v <= 10'000; ++v) {
        h.record(Micros(v));
    }
    for (const double p : {50.0, 90.0, 99.0, 99.9}) {
        const std::int64_t exact = static_cast<std::int64_t>(p / 100.0 * 10'000.0) * 1'000;
        const std::int64_t got = h.percentile(p).value().count();

        // Never below the true value — the whole point of the high edge.
        CHECK(got >= exact);
        // And never further above it than the layout's 0.781% bound.
        CHECK(static_cast<double>(got - exact) / static_cast<double>(exact) < 0.008);
    }
}

TEST_CASE("percentile never reports below the exact value, at any p") {
    // The guarantee, checked at every whole percentile rather than a few.
    std::vector<std::int64_t> samples;
    for (std::int64_t v = 1; v <= 2'000; ++v) {
        samples.push_back(v * 137);  // spread across octaves, not round numbers
    }
    Histogram h;
    for (const std::int64_t v : samples) {
        h.record(Nanos(v));
    }
    std::sort(samples.begin(), samples.end());

    for (int pi = 0; pi <= 100; ++pi) {
        const double p = static_cast<double>(pi);
        const std::size_t rank = static_cast<std::size_t>(
            std::clamp<double>(std::ceil(p / 100.0 * 2000.0), 1.0, 2000.0));
        const std::int64_t exact = samples[rank - 1];
        REQUIRE(h.percentile(p).value().count() >= exact);
    }
}

TEST_CASE("a rank in the overflow region reports max, not 60s") {
    Histogram h;
    for (int i = 0; i < 99; ++i) {
        h.record(Micros(100));
    }
    h.record(Secs(90));  // the 100th sample: past the layout's top

    CHECK(h.overflow() == 1);
    // p50 is bucketed and unaffected.
    CHECK(h.percentile(50.0).value() == Nanos(buckets::slot_high(buckets::index_of(100'000))));
    // p100 falls past every slot. Returning 60s would under-report a
    // 90-second stall by 30 seconds; max is the high end of [60s, max].
    CHECK(h.percentile(100.0).value() == Secs(90));
    CHECK(h.percentile(100.0).value() > Nanos(buckets::kMaxValue));
}

TEST_CASE("p100 is at least max, and max is the exact one to quote") {
    Histogram h;
    h.record(Micros(1500));
    // p100 comes from a slot edge, so it rounds up; max() is exact. Both are
    // correct answers to different questions, and neither is below the truth.
    CHECK(h.percentile(100.0).value() >= h.max());
    CHECK(h.max() == Nanos(1'500'000));
}
