#include "core/clock.hpp"

#include <algorithm>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

Nanos MonotonicClock::measured_resolution() {
    // The first clock read of a process is systematically slow — cold cache,
    // first call into the commpage — so discard one rather than let it become
    // the answer.
    (void)now();

    // What this measures is the smallest *observable* non-zero delta: tick
    // granularity and call overhead together, because two readings cannot be
    // closer than one call apart. That combined number is the honest one, since
    // it is the floor on any latency this rig can report.
    Nanos best = Nanos::max();
    for (int attempt = 0; attempt < 5; ++attempt) {
        const Instant a = now();
        Instant b = a;
        for (int spins = 0; b == a; ++spins) {
            // A monotonic clock that never advances is a broken platform, not a
            // slow one. Startup is the right place to say so — and an
            // unbounded spin here would hang instead of reporting.
            if (spins > 100'000'000) {
                throw Error("steady_clock did not advance over 1e8 reads");
            }
            b = now();
        }
        best = min(best, between(a, b));
    }
    return best;
}

}  // namespace dariyanaap
