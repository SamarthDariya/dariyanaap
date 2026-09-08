#include "stats/csv.hpp"

#include <iomanip>

#include "stats/buckets.hpp"

using namespace std;

namespace dariyanaap::csv {
namespace {

// No quoting or escaping anywhere in this file, and that is a claim rather
// than an omission: every column below is a number. M5 adds target and mode,
// which are strings, and it has to add escaping at the same time.
constexpr const char* kSummaryColumns =
    "samples,overflow,percentiles_bounded,duration_ns,per_second,"
    "p50_ns,p90_ns,p99_ns,p999_ns,max_ns";

}  // namespace

void write_summary_header(ostream& out) {
    out << kSummaryColumns << '\n';
}

void write_summary_row(ostream& out, const Summary& summary) {
    // fixed, not the default: at 1.2M rps the default formatting emits
    // "1.23457e+06", which most CSV readers will take as a string.
    out << summary.samples << ','
        << summary.overflow << ','
        << (summary.percentiles_bounded() ? 1 : 0) << ','
        << summary.duration.count() << ','
        << fixed << setprecision(2) << summary.per_second() << defaultfloat << ','
        << summary.p50.count() << ','
        << summary.p90.count() << ','
        << summary.p99.count() << ','
        << summary.p999.count() << ','
        << summary.max.count() << '\n';
}

void write_histogram(ostream& out, const Histogram& histogram) {
    out << "slot,low_ns,high_ns,count\n";

    for (int i = 0; i < buckets::kCount; ++i) {
        const uint64_t count = histogram.slot(i);
        if (count == 0) {
            continue;
        }
        out << i << ','
            << buckets::slot_low(i) << ','
            << buckets::slot_high(i) << ','
            << count << '\n';
    }

    if (histogram.overflow() > 0) {
        out << -1 << ','
            << buckets::kMaxValue + 1 << ','
            << histogram.max().count() << ','
            << histogram.overflow() << '\n';
    }
}

}  // namespace dariyanaap::csv
