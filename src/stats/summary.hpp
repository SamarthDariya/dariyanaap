#pragma once

#include <cstdint>
#include <optional>

#include "core/units.hpp"
#include "stats/histogram.hpp"

namespace dariyanaap {

// The reportable shape of one run's latency distribution.
//
// What is NOT here is most of the design:
//
// - No mean. DESIGN.md decision 3, enforced by a static_assert in the tests.
//
// - No error rate, and no error counts. Decision 6 says errors are counted by
//   kind — connect, write, read, timeout, protocol, non-2xx — and six of those
//   are meaningless to a histogram: "non-2xx" is an HTTP fact, and stats does
//   not know HTTP exists. They belong to load's per-run result, which will
//   carry an optional<Summary> alongside them. That layering is what keeps
//   stats linkable by a target that only wants to time itself.
//
// - No target, mode, or rig ceiling. Those describe the run rather than the
//   distribution, and they are M5's CSV-header concern.
//
// The optional in of() is the same rule as Rate, Endpoint and percentile():
// a run whose every request failed has no latency summary, and inventing one
// full of zeros would be the most flattering possible under-report. The run
// still gets reported — by load, with its error counts and no Summary.
struct Summary {
    // nullopt when nothing was recorded. Asserts on a non-positive duration:
    // a run has a duration, and dividing by it is the only way to a rate.
    static std::optional<Summary> of(const Histogram& histogram, Nanos duration);

    std::uint64_t samples = 0;
    std::uint64_t overflow = 0;
    Nanos duration{0};

    Nanos p50{0};
    Nanos p90{0};
    Nanos p99{0};
    Nanos p999{0};
    // Exact, not a slot edge — and therefore NOT the top of the ladder above.
    //
    // p999 can exceed max by up to 0.781%. That is not a bug: the percentiles
    // are slot high edges, deliberately rounded up so they never under-report,
    // while max is the true largest sample. A CSV row showing p999 > max is
    // showing the bucket bias, and whoever formats that row (M5) has to make
    // it legible rather than let it read as broken arithmetic.
    //
    // Both are kept because they answer different questions: max is the number
    // to quote for the single worst request, and p999 is the number to compare
    // between runs.
    Nanos max{0};

    double per_second() const;

    // False when part of the distribution sat above 60s, so the percentiles
    // above it are drawn from max() rather than from a slot. This is the
    // obligation percentile() created: whoever prints a Summary must print
    // this too, or the numbers read as more precise than they are.
    bool percentiles_bounded() const { return overflow == 0; }
};

}  // namespace dariyanaap
