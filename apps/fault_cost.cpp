// E4: what does a disabled fault check cost?
//
// DESIGN.md decision 9 claims a disabled check is one relaxed atomic load, and
// that this is cheap enough for a target to ship on its hot path. The claim
// matters more than it sounds: if it were false, targets would guard the calls
// with #ifdef, the fault-injecting build would differ from the measured build,
// and every number in the track would come from a different program than the
// one being reasoned about.
//
//     ./build/fault_cost [iterations]

#include <cstdio>
#include <cstdlib>

#include "core/clock.hpp"
#include "fault/knobs.hpp"

using namespace dariyanaap;
using namespace std;

namespace {

// volatile so the compiler cannot decide the loop has no effect and delete it,
// which would make the measurement zero and the claim unfalsifiable.
volatile bool g_sink = false;

double per_call_ns(uint64_t iterations, bool call_fault) {
    const MonotonicClock::Instant start = MonotonicClock::now();
    for (uint64_t i = 0; i < iterations; ++i) {
        if (call_fault) {
            fault::before_response();
            g_sink = fault::should_drop();
        } else {
            g_sink = (i & 1) != 0;
        }
    }
    return static_cast<double>(MonotonicClock::since(start).count()) /
           static_cast<double>(iterations);
}

}  // namespace

int main(int argc, char** argv) {
    const uint64_t iterations = argc > 1 ? strtoull(argv[1], nullptr, 10) : 50'000'000;

    // Warm the core first. E0 measured a 2.5x difference in the clock's own
    // resolution between a cold core and a warm one, and a per-call figure of
    // a few nanoseconds is exactly the scale that gets swamped by it.
    (void)per_call_ns(iterations / 10, true);

    fault::clear();
    const double baseline = per_call_ns(iterations, false);
    const double all_off = per_call_ns(iterations, true);

    fault::set_drop_probability(0.0);   // explicitly zero, not merely unset
    const double zero_drop = per_call_ns(iterations, true);

    printf("iterations            %llu\n", static_cast<unsigned long long>(iterations));
    printf("no fault calls        %6.3f ns/op\n", baseline);
    printf("fault linked, all off %6.3f ns/op   (+%.3f ns)\n", all_off, all_off - baseline);
    printf("drop_probability(0.0) %6.3f ns/op   (+%.3f ns)\n", zero_drop,
           zero_drop - baseline);

    // A syscall pair costs microseconds; the budget here is a few nanoseconds.
    const double worst = all_off > zero_drop ? all_off : zero_drop;
    const double added = worst - baseline;
    printf("\nadded cost %.3f ns/op — %s (budget 5 ns)\n", added,
           added < 5.0 ? "PASS" : "FAIL");
    return added < 5.0 ? 0 : 1;
}
