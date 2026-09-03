#include "core/units.hpp"

#include <cmath>
#include <sstream>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {
namespace {

// Bounds exist so interval() and due_at() can stay total. 1e9/rps must fit in
// an int64 nanosecond count, and llround() of an infinity is undefined
// behaviour, not a large number — so the only place to stop it is here, once,
// at construction. Below 0.001 rps you are not load testing; above 1e9 the
// interval rounds to zero nanoseconds and the schedule collapses.
constexpr double kMinRps = 0.001;
constexpr double kMaxRps = 1e9;

}  // namespace

Rate Rate::per_second(double rps) {
    // isfinite first: a NaN compares false against every bound, so a bounds
    // check alone would let it through.
    if (!isfinite(rps)) {
        throw UsageError("rate must be a finite number of requests per second");
    }
    if (rps < kMinRps || rps > kMaxRps) {
        throw UsageError("rate must be between 0.001 and 1e9 rps, got " + to_string(rps));
    }
    return Rate(rps);
}

Nanos Rate::interval() const {
    return Nanos(llround(1e9 / rps_));
}

Nanos Rate::due_at(uint64_t index) const {
    return Nanos(llround(static_cast<double>(index) * 1e9 / rps_));
}

string Rate::str() const {
    // ostringstream, not to_string: to_string(double) always prints six
    // decimals, so 1000 rps would appear in the CSV header as "1000.000000".
    ostringstream out;
    out << rps_ << " rps";
    return out.str();
}

}  // namespace dariyanaap
