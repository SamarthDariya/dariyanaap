#pragma once

#include <chrono>

#include "core/units.hpp"

namespace dariyanaap {

// The one clock this repo is allowed to read.
//
// steady_clock, never system_clock, and the ban is absolute rather than
// stylistic. system_clock steps backwards when NTP corrects it — and unit 10
// (dariyabarf) moves it backwards *on purpose*, to make a Snowflake generator
// emit duplicates. A rig that timed that experiment with the clock the
// experiment corrupts would report negative latencies and call them a tail.
//
// Everything funnels through here so the ban is checkable rather than
// reviewed:
//
//     grep -rn 'system_clock' src/ | grep -v '//'
//
// should print nothing. This comment is why the grep drops comment lines.
class MonotonicClock {
public:
    using Instant = std::chrono::steady_clock::time_point;

    static Instant now() { return std::chrono::steady_clock::now(); }

    // since() is for "how long has this been running"; between() is for a
    // request, whose two endpoints are both read explicitly on the hot path.
    static Nanos since(Instant start) { return now() - start; }
    static Nanos between(Instant start, Instant end) { return end - start; }

    // The smallest interval this machine can actually distinguish, measured at
    // startup rather than assumed. Printed in the CSV header: a p99 floor of
    // 800ns means something different on a clock that ticks in 1us steps than
    // on one that ticks in 40ns steps, and decision 7 says the rig publishes
    // its own limits instead of letting them be inferred.
    static Nanos measured_resolution();
};

// Holds a start instant so "has the run lasted long enough" reads as a
// question about the run rather than as clock arithmetic.
class Stopwatch {
public:
    Stopwatch() : start_(MonotonicClock::now()) {}

    Nanos elapsed() const { return MonotonicClock::since(start_); }
    MonotonicClock::Instant start() const { return start_; }

private:
    MonotonicClock::Instant start_;
};

}  // namespace dariyanaap
