// The M1 verifier: does the histogram tell the truth about a known
// distribution, and does it cost what buckets.hpp claims?
//
// Track rule 3 — every repo ships a benchmark, and if you cannot produce a
// number you did not finish the unit. This is that number for M1, and it fills
// in BREAK.md E1.
//
//     ./build/hist_verify [samples]        default 1,000,000
//
// The distribution is BREAK.md E1's: lognormal around a couple of hundred
// microseconds, plus a deliberate 1-in-1000 spike at 500ms. The spike is the
// point — a histogram that is accurate in the body and wrong in the tail is
// useless here, because every experiment in the track lives in the tail.
//
// Seeded fixed, so two runs on the same machine are comparable and a
// regression is a regression rather than luck.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "core/clock.hpp"
#include "stats/histogram.hpp"

using namespace dariyanaap;
using namespace std;

int main(int argc, char** argv) {
    const size_t samples = argc > 1 ? strtoul(argv[1], nullptr, 10) : 1'000'000;

    mt19937_64 rng(20260902);
    lognormal_distribution<double> body(log(200'000.0), 0.6);  // ~200us, in ns
    uniform_int_distribution<int> spike(1, 1000);

    vector<int64_t> exact;
    exact.reserve(samples);
    for (size_t i = 0; i < samples; ++i) {
        const int64_t ns = spike(rng) == 1 ? 500'000'000
                                           : static_cast<int64_t>(body(rng));
        exact.push_back(max<int64_t>(ns, 1));
    }

    // Time only record(), on values already in hand, so the number is the
    // histogram's cost and not the generator's.
    Histogram h;
    const MonotonicClock::Instant started = MonotonicClock::now();
    for (const int64_t ns : exact) {
        h.record(Nanos(ns));
    }
    const Nanos elapsed = MonotonicClock::since(started);

    sort(exact.begin(), exact.end());

    printf("samples          %zu\n", samples);
    printf("record() cost    %.1f ns/op\n",
           static_cast<double>(elapsed.count()) / static_cast<double>(samples));
    printf("histogram        %zu bytes, fixed\n", sizeof(Histogram));
    printf("exact percentiles need %zu bytes of samples (%.0fx more)\n",
           samples * sizeof(int64_t),
           static_cast<double>(samples * sizeof(int64_t)) / sizeof(Histogram));
    printf("overflow         %llu\n", static_cast<unsigned long long>(h.overflow()));
    printf("\n%12s %14s %14s %9s\n", "percentile", "exact ns", "reported ns", "error");

    double worst = 0.0;
    for (const double p : {50.0, 90.0, 99.0, 99.9, 100.0}) {
        // The same rank formula Histogram uses, so this compares like with like.
        const size_t rank = static_cast<size_t>(
            clamp<double>(ceil(p / 100.0 * static_cast<double>(samples)),
                          1.0, static_cast<double>(samples)));
        const int64_t truth = exact[rank - 1];
        const int64_t got = h.percentile(p).value().count();
        const double error = static_cast<double>(got - truth) / static_cast<double>(truth);
        worst = max(worst, fabs(error));
        printf("%11.1f%% %14lld %14lld %8.4f%%\n", p,
               static_cast<long long>(truth), static_cast<long long>(got), error * 100.0);
        if (got < truth) {
            printf("  FAIL: reported below the true value — the never-under-report "
                   "guarantee is broken\n");
            return 1;
        }
    }

    const double bound = 1.0 / static_cast<double>(buckets::kSubBuckets);
    printf("\nworst error %.4f%%, bound %.4f%%: %s\n", worst * 100.0, bound * 100.0,
           worst <= bound ? "PASS" : "FAIL");
    return worst <= bound ? 0 : 1;
}
