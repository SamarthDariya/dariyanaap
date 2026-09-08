#pragma once

#include <array>
#include <cassert>
#include <cstdint>

#include "core/units.hpp"
#include "stats/buckets.hpp"

namespace dariyanaap {

// A latency histogram: 3,808 counters, 29.75KB, fixed for the life of the run.
//
// One of these per thread, merged once at the end (DESIGN.md decision 5), so
// nothing here is atomic and nothing here locks. record() is the only thing on
// the hot path and it does not allocate, branch on anything but the overflow
// case, or touch memory outside this object.
class Histogram {
public:
    // Two kinds of out-of-range value, with two different answers.
    //
    // A NEGATIVE latency is a broken caller. MonotonicClock is monotonic, so
    // end - start cannot be negative unless the two came from different
    // clocks or in the wrong order. That is a bug, so it asserts rather than
    // being counted: reporting it would let the run continue producing numbers
    // computed from a corrupted structure, which is the worst thing a
    // measuring tool can do.
    //
    // A latency ABOVE 60s is a real measurement the layout cannot hold. It is
    // counted separately and never clamped into the top slot, because clamping
    // would report a 90-second stall as 60 seconds — an under-report, and this
    // repo's whole thesis is that under-reporting is the failure mode that
    // matters. max_ keeps it exactly.
    void record(Nanos latency) {
        const std::int64_t ns = latency.count();
        assert(ns >= 0 && "negative latency: the caller mixed up two clocks");

        ++count_;
        if (ns > max_) {
            max_ = ns;
        }
        if (ns > buckets::kMaxValue) [[unlikely]] {
            ++overflow_;
            return;
        }
        ++slots_[static_cast<std::size_t>(buckets::index_of(ns))];
    }

    // Every sample recorded, including the ones too large to bucket.
    std::uint64_t count() const { return count_; }

    // Samples above 60s. Non-zero means percentile() cannot place part of the
    // distribution, and whoever reports the run has to say so.
    std::uint64_t overflow() const { return overflow_; }

    // Exact, not bucketed. The single worst latency is a number people quote,
    // and rounding it up by 0.78% to a slot edge would be a needless lie in
    // the one place the true value was already in hand.
    Nanos max() const { return Nanos(max_); }

    std::uint64_t slot(int index) const {
        return slots_[static_cast<std::size_t>(index)];
    }

    // Deliberately absent: mean(). See DESIGN.md decision 3. The mean averages
    // a bimodal distribution whose two modes are "the fast path" and "the
    // problem", and reports neither. stats_tests.cpp asserts it stays absent.

private:
    std::array<std::uint64_t, buckets::kCount> slots_{};
    std::uint64_t count_ = 0;
    std::uint64_t overflow_ = 0;
    std::int64_t max_ = 0;
};

}  // namespace dariyanaap
