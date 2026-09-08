#pragma once

#include <ostream>

#include "stats/histogram.hpp"
#include "stats/summary.hpp"

namespace dariyanaap::csv {

// CSV is the rig's only output, so it is data rather than a report: values are
// nanoseconds, unformatted and lossless, and the human-facing rounding into
// microseconds happens in whatever reads this.
//
// That corrects units.hpp's "reported latency is microseconds", which does not
// survive decision 7. The rig's own p99 floor is tens of nanoseconds, and an
// integer microsecond column would record the number this whole repo exists to
// publish as "0".
//
// Everything takes an ostream rather than a path: a formatter that can only be
// exercised by writing to disk does not get tested, and these files are the
// interface every later repo reads.

// Split so a sweep — the same target at 1, 10, 100, 500 connections — writes
// one header and one row per run into a single file. That file is the input to
// every "throughput vs concurrency" plot in the track.
void write_summary_header(std::ostream& out);
void write_summary_row(std::ostream& out, const Summary& summary);

// Header and rows together: one histogram file per run.
//
// Only non-empty slots are written, because 3,808 rows of mostly zeros is not
// more honest, just larger. Each row carries its own [low_ns, high_ns], so the
// file is self-describing and can be re-percentiled or merged with another run
// months later without this binary — which is the claim DESIGN.md decision 4
// makes for keeping raw counts instead of only percentiles.
//
// Samples above 60s have no slot, so they are written as a final row with
// slot = -1 and high_ns = the exact max. The invariant a reader can rely on:
// the count column sums to the run's total sample count.
void write_histogram(std::ostream& out, const Histogram& histogram);

}  // namespace dariyanaap::csv
