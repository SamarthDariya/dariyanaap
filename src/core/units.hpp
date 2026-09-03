#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace dariyanaap {

// Durations are chrono types, not hand-rolled wrappers. DESIGN.md decision 12
// wants strong duration types so f(timeout, rate) cannot be called with its
// arguments swapped; chrono already gives exactly that, with conversions
// checked at compile time. Rolling our own would be worse code for the same
// property.
//
// Internal arithmetic is nanoseconds; reported latency is microseconds. The
// histogram's bottom bucket is 1us, so nanosecond resolution exists only to
// keep the open-loop schedule exact — see due_at below.
using Nanos  = std::chrono::nanoseconds;
using Micros = std::chrono::microseconds;
using Millis = std::chrono::milliseconds;
using Secs   = std::chrono::seconds;

// Offered load, in requests per second.
//
// A Rate is always strictly positive. There is no "unthrottled" Rate and no
// default constructor: closed-loop mode does not have a rate at all, so it
// carries no Rate rather than a Rate of zero. Absence is std::optional<Rate>,
// which is honest about being absent.
//
// The payoff is that interval() and due_at() are total functions — they cannot
// fail, so they cannot throw, so they are safe on the M3 send path where they
// are called once per request.
class Rate {
public:
    // The only way to make one. Throws UsageError unless rps is finite and
    // within [0.001, 1e9]; those bounds are what let the two functions below
    // stay total, and units.cpp says why.
    static Rate per_second(double rps);

    double rps() const { return rps_; }

    // Gap between consecutive requests.
    Nanos interval() const;

    // When request `index` is due, measured from the start of the run.
    //
    // Computed as index/rate, never by accumulating interval(). interval()
    // rounds once and the schedule then adds that same rounding error N times,
    // so the error is systematic: it compounds linearly and always in one
    // direction. Rounding here instead keeps it at ~1ns forever, whatever N is.
    //
    // Measured, at 3,000,000 requests: 3 rps drifts +1.000ms when accumulated,
    // 7 rps drifts -0.429ms, and 100,000 rps drifts exactly nothing, because
    // 10,000ns divides a second evenly. That last one is why the bug survives
    // testing at round rates and only shows up at 3 or 7.
    //
    // Open-loop latency is measured from the intended send time, so a drifting
    // schedule is reported as target latency that no target caused.
    Nanos due_at(std::uint64_t index) const;

    std::string str() const;

private:
    explicit Rate(double rps) : rps_(rps) {}
    double rps_;  // invariant: finite and > 0
};

}  // namespace dariyanaap
