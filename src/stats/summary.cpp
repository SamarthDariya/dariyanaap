#include "stats/summary.hpp"

#include <cassert>

using namespace std;

namespace dariyanaap {

optional<Summary> Summary::of(const Histogram& histogram, Nanos duration) {
    assert(duration.count() > 0 && "a run has a positive duration");

    if (histogram.count() == 0) {
        return nullopt;
    }

    Summary out;
    out.samples = histogram.count();
    out.overflow = histogram.overflow();
    out.duration = duration;

    // value() rather than value_or: count() > 0 was just checked, so a missing
    // percentile here would be a bug in Histogram, and throwing beats
    // substituting a zero that would then be reported as a latency.
    out.p50 = histogram.percentile(50.0).value();
    out.p90 = histogram.percentile(90.0).value();
    out.p99 = histogram.percentile(99.0).value();
    out.p999 = histogram.percentile(99.9).value();
    out.max = histogram.max();
    return out;
}

double Summary::per_second() const {
    // Samples per second of wall clock. Not "successful requests per second" —
    // this counts what was recorded, and only load knows what was attempted.
    return static_cast<double>(samples) /
           (static_cast<double>(duration.count()) / 1e9);
}

}  // namespace dariyanaap
