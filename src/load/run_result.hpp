#pragma once

#include <cstdint>
#include <optional>

#include "core/units.hpp"
#include "stats/summary.hpp"

namespace dariyanaap {

// The six ways a request can fail, counted apart from one another.
//
// DESIGN.md decision 6: never one error rate. "3% errors" cannot distinguish
// the backend refusing connections from the backend returning 503 instantly
// from the backend hanging until we gave up — and those three have opposite
// implications for a load balancer, which is unit 2's entire subject.
struct ErrorCounts {
    std::uint64_t connect = 0;   // never got a connection
    std::uint64_t write = 0;     // connected, could not send
    std::uint64_t read = 0;      // sent, the stream broke before a whole reply
    std::uint64_t timeout = 0;   // sent, nothing came back in time
    std::uint64_t protocol = 0;  // a reply arrived that cannot be a reply
    std::uint64_t rejected = 0;  // a whole, well-formed reply saying no (non-2xx)

    std::uint64_t total() const {
        return connect + write + read + timeout + protocol + rejected;
    }

    void merge(const ErrorCounts& other) {
        connect += other.connect;
        write += other.write;
        read += other.read;
        timeout += other.timeout;
        protocol += other.protocol;
        rejected += other.rejected;
    }
};

// Everything one run produced.
//
// WHICH REQUESTS ARE IN THE HISTOGRAM, and why — this was implicit until now:
//
//   In:  successes; non-2xx replies; timeouts, recorded AT the timeout value.
//   Out: connect failures, write failures, read failures, protocol errors.
//
// Timeouts are in because leaving them out is coordinated omission arriving
// early: the slowest requests of the run would be the only ones missing from
// the distribution (chunk 2.3).
//
// Non-2xx replies are in because latency is a property of time, not of
// semantics, and unit 9 needs it that way — when a circuit breaker trips and
// starts failing fast, the caller's p99 genuinely improves, and a histogram
// that excluded the fast failures would hide the effect the breaker exists to
// produce. The `rejected` counter is what tells a reader the distribution is a
// mixture.
//
// The rest are out because they have no reply to time. A connect failure has
// no request latency at all, and a protocol error's duration measures how long
// the target took to say something unintelligible.
struct RunResult {
    // Requests the runner started. Counted directly rather than derived,
    // because the derivation is subtle enough to get wrong quietly.
    std::uint64_t attempted = 0;

    ErrorCounts errors;

    // Absent when nothing produced a duration. Not a Summary full of zeros:
    // a run whose every connect was refused has no distribution, and inventing
    // one would be the most flattering possible under-report (chunk 1.6).
    std::optional<Summary> latency;

    Nanos duration{0};

    // Requests that reached the histogram.
    std::uint64_t timed() const { return latency ? latency->samples : 0; }

    // Failures with no reply to time, so not in the histogram.
    std::uint64_t untimed() const {
        return errors.connect + errors.write + errors.read + errors.protocol;
    }

    // Does the accounting add up?
    //
    // Worth having as a function rather than a comment: the runner increments
    // these from several places, and a request counted as attempted but landing
    // in neither the histogram nor an untimed error is a request that vanished.
    // A rig that loses requests silently reports a throughput it did not
    // achieve, which is the one failure this repo cannot tolerate.
    bool consistent() const { return attempted == timed() + untimed(); }
};

}  // namespace dariyanaap
