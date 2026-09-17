#pragma once

#include <ostream>
#include <string>
#include <vector>

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

// One value, quoted if it needs to be.
//
// Chunk 1.7 promised this: "M5 adds target and mode, which are strings, and it
// has to add escaping at the same time." The CLI is where that lands. A target
// name containing a comma would otherwise shift every column to its right,
// and the resulting file parses cleanly into the wrong numbers.
std::string quoted(const std::string& value);

// Split so a sweep — the same target at 1, 10, 100, 500 connections — writes
// one header and one row per run into a single file. That file is the input to
// every "throughput vs concurrency" plot in the track.
void write_summary_header(std::ostream& out);
void write_summary_row(std::ostream& out, const Summary& summary);

// A row for a run that produced no distribution — every connect refused, say.
//
// Lives here rather than in the caller so it cannot drift from the header. A
// caller writing its own row of zeros would silently misalign the moment a
// column is added, and the file would still parse.
void write_absent_summary_row(std::ostream& out);

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

// One row per elapsed second: when the run degraded, not just by how much.
//
// A second with no samples still gets a row, with a count of zero. That is the
// most informative row in the file — a target that stopped answering entirely
// looks identical to a missing row otherwise, and "the file has a gap" and
// "the service was down" are different findings.
void write_timeseries(std::ostream& out, const std::vector<Histogram>& seconds);

}  // namespace dariyanaap::csv
