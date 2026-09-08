#include "stats/histogram.hpp"

#include <algorithm>
#include <cmath>

using namespace std;

namespace dariyanaap {

optional<Nanos> Histogram::percentile(double p) const {
    assert(p >= 0.0 && p <= 100.0 && "percentile must be in [0, 100]");

    if (count_ == 0) {
        return nullopt;
    }

    // The rank of the sample being asked for. ceil, and never below 1: p=0
    // must name the smallest sample recorded, not rank zero, which no sample
    // occupies. Clamped at the top so floating-point rounding on p=100 cannot
    // ask for a sample that does not exist.
    // std::clamp, qualified: inside a member function `max` resolves to
    // Histogram::max before std::max, `using namespace std` or not. One of the
    // sharp edges of the house style's using-directive.
    const uint64_t target = std::clamp<uint64_t>(
        static_cast<uint64_t>(ceil(p / 100.0 * static_cast<double>(count_))),
        1, count_);

    uint64_t running = 0;
    for (int i = 0; i < buckets::kCount; ++i) {
        running += slots_[static_cast<size_t>(i)];
        if (running >= target) {
            return Nanos(buckets::slot_high(i));
        }
    }

    // Falling out of the loop means the rank lies in the overflow region: more
    // than (count - overflow) samples were needed, so the answer is above 60s.
    // max_ is exact and is the high end of the interval the answer lives in,
    // which keeps the never-under-report guarantee intact.
    return Nanos(max_);
}

void Histogram::merge(const Histogram& other) {
    // Merging a histogram into itself would double every count. It is almost
    // certainly a loop indexing mistake rather than an intent, and this is a
    // cold path, so the check is free.
    assert(this != &other && "merging a histogram into itself doubles it");

    for (size_t i = 0; i < slots_.size(); ++i) {
        slots_[i] += other.slots_[i];
    }
    count_ += other.count_;
    overflow_ += other.overflow_;
    max_ = std::max(max_, other.max_);
}

}  // namespace dariyanaap
