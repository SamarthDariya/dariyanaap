#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

#include "core/clock.hpp"
#include "stats/histogram.hpp"

namespace dariyanaap {

// Latency bucketed by wall-clock second, so "when did it degrade" is
// answerable (DESIGN.md's output schema). Units 2 and 9 need it: a backend
// killed mid-run and a circuit breaker tripping and recovering are both events
// at a time, and a single run-wide p99 cannot show either.
//
// The hard part is not the bucketing, it is doing it without a lock on the
// request path. Decision 5 keeps histograms per-thread precisely because a
// shared one turns the modal counter into a contended cache line, and a mutex
// per request would be worse still.
//
// So each worker owns one Writer holding a single second's histogram. At a
// second boundary it merges that into the shared vector under a lock and
// starts again — one lock per worker per second, which at 1000 workers is a
// thousand acquisitions a second against millions of requests.
class TimeSeries {
public:
    explicit TimeSeries(MonotonicClock::Instant start) : start_(start) {}

    class Writer {
    public:
        explicit Writer(TimeSeries& owner) : owner_(owner) {}

        // `now` is the instant the request finished; the caller has already
        // read the clock for the latency, so this costs no extra reading.
        void record(MonotonicClock::Instant now, Nanos latency) {
            const std::uint64_t second = owner_.second_of(now);
            if (second != current_) {
                flush();
                current_ = second;
            }
            pending_.record(latency);
        }

        // Hand the current second to the shared series. Called at a boundary
        // and once more when the worker stops, or the last partial second
        // would be dropped — and in a run that ends mid-degradation that is
        // the second worth having.
        void flush() {
            if (pending_.count() > 0) {
                owner_.absorb(current_, pending_);
                pending_ = Histogram();
            }
        }

    private:
        TimeSeries& owner_;
        Histogram pending_;
        std::uint64_t current_ = 0;
    };

    Writer writer() { return Writer(*this); }

    // One histogram per elapsed second, in order. Read after every worker has
    // stopped and flushed.
    const std::vector<Histogram>& seconds() const { return seconds_; }

private:
    std::uint64_t second_of(MonotonicClock::Instant now) const {
        const Nanos since = MonotonicClock::between(start_, now);
        return since.count() <= 0 ? 0
                                  : static_cast<std::uint64_t>(since.count() / 1'000'000'000);
    }

    void absorb(std::uint64_t second, const Histogram& histogram) {
        const std::lock_guard<std::mutex> held(lock_);
        if (seconds_.size() <= second) {
            seconds_.resize(second + 1);
        }
        seconds_[second].merge(histogram);
    }

    MonotonicClock::Instant start_;
    std::mutex lock_;
    std::vector<Histogram> seconds_;
};

}  // namespace dariyanaap
