#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "stats/buckets.hpp"
#include "stats/histogram.hpp"
#include "stats/csv.hpp"
#include "stats/summary.hpp"

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

// ---------------------------------------------------------------------------
// Chunk 1.5 — merge()
// ---------------------------------------------------------------------------

TEST_CASE("merging preserves every field, including the ones easy to forget") {
    Histogram a;
    a.record(Micros(100));
    a.record(Secs(90));  // overflow, and the larger max

    Histogram b;
    b.record(Micros(200));
    b.record(Micros(200));

    a.merge(b);
    CHECK(a.count() == 4);
    CHECK(a.overflow() == 1);
    CHECK(a.max() == Secs(90));
    CHECK(a.slot(buckets::index_of(100'000)) == 1);
    CHECK(a.slot(buckets::index_of(200'000)) == 2);
}

TEST_CASE("merging an empty histogram changes nothing") {
    Histogram a;
    a.record(Micros(7));
    const Histogram empty;

    a.merge(empty);
    CHECK(a.count() == 1);
    CHECK(a.max() == Micros(7));
    CHECK(a.percentile(99.0).value() == Nanos(buckets::slot_high(buckets::index_of(7'000))));
}

TEST_CASE("merge order does not change the result") {
    // Threads finish in whatever order they finish, so a merge that depended
    // on order would make a run non-reproducible.
    const std::vector<std::int64_t> xs{5, 5, 900, 1'000'000};
    const std::vector<std::int64_t> ys{7, 800, 2'000'000};
    const std::vector<std::int64_t> zs{60'000'000'001};  // overflow

    auto build = [](const std::vector<std::int64_t>& vs) {
        Histogram h;
        for (const std::int64_t v : vs) h.record(Nanos(v));
        return h;
    };

    Histogram forward = build(xs);
    forward.merge(build(ys));
    forward.merge(build(zs));

    Histogram backward = build(zs);
    backward.merge(build(ys));
    backward.merge(build(xs));

    CHECK(forward.count() == backward.count());
    CHECK(forward.overflow() == backward.overflow());
    CHECK(forward.max() == backward.max());
    for (int i = 0; i < buckets::kCount; ++i) {
        REQUIRE(forward.slot(i) == backward.slot(i));
    }
    CHECK(forward.percentile(99.0).value() == backward.percentile(99.0).value());
}

TEST_CASE("per-thread histograms merged after joining equal one single-threaded run") {
    // This is the claim DESIGN.md decision 5 rests on, and the reason TSan is
    // wired into this repo at all: N threads record into their OWN histogram
    // with no synchronisation, and the merged result must be bit-identical to
    // recording every sample on one thread.
    constexpr int kThreads = 8;
    constexpr int kPerThread = 50'000;

    // A deterministic sample per (thread, i), so both sides see the same set.
    auto sample = [](int t, int i) -> std::int64_t {
        return 1 + static_cast<std::int64_t>(t + 1) * 37 + static_cast<std::int64_t>(i) * 811;
    };

    std::vector<Histogram> per_thread(kThreads);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&per_thread, &sample, t] {
            for (int i = 0; i < kPerThread; ++i) {
                per_thread[static_cast<std::size_t>(t)].record(Nanos(sample(t, i)));
            }
        });
    }
    for (std::thread& th : threads) th.join();

    Histogram merged;
    for (Histogram& h : per_thread) merged.merge(h);

    Histogram single;
    for (int t = 0; t < kThreads; ++t) {
        for (int i = 0; i < kPerThread; ++i) {
            single.record(Nanos(sample(t, i)));
        }
    }

    CHECK(merged.count() == static_cast<std::uint64_t>(kThreads) * kPerThread);
    CHECK(merged.count() == single.count());
    CHECK(merged.max() == single.max());
    CHECK(merged.overflow() == single.overflow());
    for (int i = 0; i < buckets::kCount; ++i) {
        REQUIRE(merged.slot(i) == single.slot(i));
    }
    for (const double p : {50.0, 90.0, 99.0, 99.9, 100.0}) {
        REQUIRE(merged.percentile(p).value() == single.percentile(p).value());
    }
}

// ---------------------------------------------------------------------------
// Chunk 1.6 — Summary
// ---------------------------------------------------------------------------

// Decision 6: no single error rate on the summary. Same enforcement style as
// the mean — a comment would not have stopped anyone.
template <class T>
concept HasErrorRate = requires(const T& t) { t.error_rate(); };
static_assert(!HasErrorRate<Summary>, "DESIGN.md decision 6: errors are counted by kind, in load");

template <class T>
concept SummaryHasMean = requires(const T& t) { t.mean(); };
static_assert(!SummaryHasMean<Summary>, "DESIGN.md decision 3: never report a mean");

TEST_CASE("a run with nothing recorded has no latency summary") {
    const Histogram empty;
    CHECK_FALSE(Summary::of(empty, Secs(10)).has_value());
    // The run still gets reported — by load, with its error counts. What does
    // not exist is a distribution, and zeros would be a flattering invention.
}

TEST_CASE("a summary carries the distribution and the duration it came from") {
    Histogram h;
    for (std::int64_t v = 1; v <= 1'000; ++v) {
        h.record(Micros(v));
    }
    const Summary s = Summary::of(h, Secs(2)).value();

    CHECK(s.samples == 1'000);
    CHECK(s.overflow == 0);
    CHECK(s.duration == Secs(2));
    CHECK(s.percentiles_bounded());

    CHECK(s.p50 <= s.p90);
    CHECK(s.p90 <= s.p99);
    CHECK(s.p99 <= s.p999);

    // max is the histogram's exact value, not a slot edge.
    CHECK(s.max == Micros(1'000));

    // And so max is NOT the top of the ladder: p999 is a slot's high edge,
    // rounded up so it never under-reports, and here it exceeds the true
    // largest sample. Asserting p999 <= max would have been asserting that
    // the never-under-report guarantee does not hold.
    CHECK(s.p999 > s.max);
    CHECK(static_cast<double>((s.p999 - s.max).count()) /
              static_cast<double>(s.max.count()) < 0.008);
    // The real upper bound of the ladder is p100, which is >= max by design.
    CHECK(h.percentile(100.0).value() >= s.max);
}

TEST_CASE("throughput is samples over wall clock, not over successes") {
    Histogram h;
    for (int i = 0; i < 5'000; ++i) {
        h.record(Micros(10));
    }
    const Summary s = Summary::of(h, Secs(2)).value();
    CHECK(s.per_second() == doctest::Approx(2'500.0));

    const Summary half = Summary::of(h, Millis(500)).value();
    CHECK(half.per_second() == doctest::Approx(10'000.0));
}

TEST_CASE("overflow makes the summary say its percentiles are unbounded") {
    Histogram h;
    for (int i = 0; i < 999; ++i) {
        h.record(Micros(100));
    }
    h.record(Secs(90));

    const Summary s = Summary::of(h, Secs(1)).value();
    CHECK(s.overflow == 1);
    CHECK_FALSE(s.percentiles_bounded());  // the obligation percentile() created
    CHECK(s.max == Secs(90));
    // p99.9 is the 1000th of 1000 samples, so it lands past every slot.
    CHECK(s.p999 == Secs(90));
    // ...while p99 is still bucketed and unaffected.
    CHECK(s.p99 == Nanos(buckets::slot_high(buckets::index_of(100'000))));
}

TEST_CASE("a summary of a merged histogram equals a summary of the whole") {
    // Summary must not care how the histogram was assembled, or a threaded run
    // and a single-threaded one would report differently.
    Histogram a;
    Histogram b;
    Histogram whole;
    for (std::int64_t v = 1; v <= 500; ++v) {
        a.record(Micros(v));
        whole.record(Micros(v));
    }
    for (std::int64_t v = 501; v <= 1'000; ++v) {
        b.record(Micros(v));
        whole.record(Micros(v));
    }
    a.merge(b);

    const Summary merged = Summary::of(a, Secs(1)).value();
    const Summary single = Summary::of(whole, Secs(1)).value();
    CHECK(merged.samples == single.samples);
    CHECK(merged.p50 == single.p50);
    CHECK(merged.p99 == single.p99);
    CHECK(merged.p999 == single.p999);
    CHECK(merged.max == single.max);
}

// ---------------------------------------------------------------------------
// Chunk 1.7 — CSV
// ---------------------------------------------------------------------------

namespace {

std::vector<std::string> split(const std::string& line, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i == line.size() || line[i] == sep) {
            out.push_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        out.push_back(line);
    }
    return out;
}

Histogram uniform(std::int64_t from, std::int64_t to) {
    Histogram h;
    for (std::int64_t v = from; v <= to; ++v) {
        h.record(Micros(v));
    }
    return h;
}

}  // namespace

TEST_CASE("the summary header names exactly as many columns as the row has fields") {
    // The bug this catches is adding a field and forgetting the header, which
    // silently shifts every column in every downstream plot.
    const Histogram h = uniform(1, 100);
    std::ostringstream out;
    csv::write_summary_header(out);
    csv::write_summary_row(out, Summary::of(h, Secs(1)).value());

    const std::vector<std::string> rows = lines_of(out.str());
    REQUIRE(rows.size() == 2);
    CHECK(split(rows[0], ',').size() == split(rows[1], ',').size());
    CHECK(split(rows[0], ',').size() == 10);
}

TEST_CASE("a sweep writes one header and one row per run") {
    std::ostringstream out;
    csv::write_summary_header(out);
    for (const int connections : {1, 10, 100}) {
        const Histogram h = uniform(1, connections * 10);
        csv::write_summary_row(out, Summary::of(h, Secs(1)).value());
    }
    CHECK(lines_of(out.str()).size() == 4);  // header + 3 runs
}

TEST_CASE("throughput is written as a plain decimal, not in exponent form") {
    Histogram h;
    for (int i = 0; i < 1'200'000; ++i) {
        h.record(Nanos(500));
    }
    std::ostringstream out;
    csv::write_summary_row(out, Summary::of(h, Secs(1)).value());
    // "1.2e+06" would be read as a string by most CSV consumers.
    CHECK(out.str().find("e+") == std::string::npos);
    CHECK(out.str().find("1200000.00") != std::string::npos);
}

TEST_CASE("the histogram file omits empty slots but keeps every sample") {
    const Histogram h = uniform(1, 1'000);
    std::ostringstream out;
    csv::write_histogram(out, h);

    const std::vector<std::string> rows = lines_of(out.str());
    CHECK(rows[0] == "slot,low_ns,high_ns,count");
    // Far fewer rows than slots: 1,000 samples cannot occupy 3,808 slots.
    CHECK(rows.size() - 1 < static_cast<std::size_t>(buckets::kCount));

    std::uint64_t total = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        total += std::stoull(split(rows[i], ',')[3]);
    }
    CHECK(total == h.count());  // the invariant the header promises
}

TEST_CASE("a run can be re-percentiled from its CSV alone") {
    // This is decision 4's reason for storing raw counts instead of only
    // percentiles: months later, without this binary, the file is enough.
    const Histogram h = uniform(1, 10'000);
    std::ostringstream out;
    csv::write_histogram(out, h);

    // Rebuild the distribution from the file and recompute p99 by hand.
    const std::vector<std::string> rows = lines_of(out.str());
    std::uint64_t total = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        total += std::stoull(split(rows[i], ',')[3]);
    }
    const std::uint64_t target = static_cast<std::uint64_t>(
        std::ceil(0.99 * static_cast<double>(total)));

    std::uint64_t running = 0;
    std::int64_t p99_from_file = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const std::vector<std::string> f = split(rows[i], ',');
        running += std::stoull(f[3]);
        if (running >= target) {
            p99_from_file = std::stoll(f[2]);  // high_ns, same edge percentile() uses
            break;
        }
    }
    CHECK(p99_from_file == h.percentile(99.0).value().count());
}

TEST_CASE("samples past 60s get a row of their own, not a silent drop") {
    Histogram h;
    for (int i = 0; i < 10; ++i) {
        h.record(Micros(100));
    }
    h.record(Secs(90));

    std::ostringstream out;
    csv::write_histogram(out, h);
    const std::vector<std::string> rows = lines_of(out.str());
    const std::vector<std::string> last = split(rows.back(), ',');

    CHECK(last[0] == "-1");                              // no slot holds it
    CHECK(std::stoll(last[1]) == buckets::kMaxValue + 1);
    // Nanos(Secs(90)), not Secs(90).count(): count() returns the value in the
    // duration's OWN units, so Secs(90).count() is 90. Calling count() throws
    // away the unit checking that made chrono the right choice in units.hpp,
    // which is why the CSV writer is the only place in the repo that calls it.
    CHECK(std::stoll(last[2]) == Nanos(Secs(90)).count());  // the exact max
    CHECK(last[3] == "1");

    std::uint64_t total = 0;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        total += std::stoull(split(rows[i], ',')[3]);
    }
    CHECK(total == h.count());  // still sums, overflow included
}

TEST_CASE("an empty histogram writes a header and nothing else") {
    const Histogram empty;
    std::ostringstream out;
    csv::write_histogram(out, empty);
    CHECK(lines_of(out.str()).size() == 1);
}

// ---------------------------------------------------------------------------
// Chunk 1.8 — the verifier, as a regression test.
//
// apps/hist_verify.cpp is the benchmark that produces BREAK.md E1's numbers.
// This is the same experiment at a smaller N, wired into ctest, so the bound
// is guarded on every build rather than only when someone remembers to run the
// tool. The distribution is deliberately not uniform: 1.4's tests use evenly
// spread values, and a tail bug can hide in those.
// ---------------------------------------------------------------------------

TEST_CASE("a lognormal body with a rare spike stays inside the bound") {
    constexpr std::size_t kSamples = 100'000;

    std::mt19937_64 rng(20260902);  // fixed: a regression must be a regression
    std::lognormal_distribution<double> body(std::log(200'000.0), 0.6);
    std::uniform_int_distribution<int> spike(1, 1000);

    std::vector<std::int64_t> exact;
    exact.reserve(kSamples);
    Histogram h;
    for (std::size_t i = 0; i < kSamples; ++i) {
        const std::int64_t ns = spike(rng) == 1
                                    ? 500'000'000
                                    : static_cast<std::int64_t>(body(rng));
        const std::int64_t clamped = std::max<std::int64_t>(ns, 1);
        exact.push_back(clamped);
        h.record(Nanos(clamped));
    }
    std::sort(exact.begin(), exact.end());

    CHECK(h.count() == kSamples);
    CHECK(h.overflow() == 0);  // 500ms is well inside the 60s range

    const double bound = 1.0 / static_cast<double>(buckets::kSubBuckets);
    for (const double p : {50.0, 90.0, 99.0, 99.9, 100.0}) {
        const std::size_t rank = static_cast<std::size_t>(
            std::clamp<double>(std::ceil(p / 100.0 * static_cast<double>(kSamples)),
                               1.0, static_cast<double>(kSamples)));
        const std::int64_t truth = exact[rank - 1];
        const std::int64_t got = h.percentile(p).value().count();

        REQUIRE(got >= truth);  // never under-reports, in the tail too
        REQUIRE(static_cast<double>(got - truth) / static_cast<double>(truth) <= bound);
    }
}

TEST_CASE("memory does not grow with sample count") {
    // The claim that makes the whole layout worth it: 10x the samples, same
    // bytes. Keeping every sample instead would be 8 bytes each.
    Histogram small;
    Histogram large;
    for (int i = 0; i < 1'000; ++i) small.record(Micros(100 + i % 50));
    for (int i = 0; i < 100'000; ++i) large.record(Micros(100 + i % 50));

    CHECK(small.count() == 1'000);
    CHECK(large.count() == 100'000);
    CHECK(sizeof(small) == sizeof(large));
    static_assert(sizeof(Histogram) == buckets::kCount * sizeof(std::uint64_t) + 24);
}
