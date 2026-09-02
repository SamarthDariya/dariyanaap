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
    // The only way to make one. Throws UsageError unless rps is finite and > 0.
    static Rate per_second(double rps);

    double rps() const { return rps_; }

    // Gap between consecutive requests.
    Nanos interval() const;

    // When request `index` is due, measured from the start of the run.
    // Computed as index/rate, never by accumulating interval() — the reason is
    // in the test that comes with chunk 0.4, and it is worth a millisecond.
    Nanos due_at(std::uint64_t index) const;

    std::string str() const;

private:
    explicit Rate(double rps) : rps_(rps) {}
    double rps_;  // invariant: finite and > 0
};

}  // namespace dariyanaap
