#pragma once

#include <atomic>
#include <cstdint>

#include "core/clock.hpp"
#include "core/units.hpp"

namespace dariyanaap {

// The open-loop schedule: request i is due at start + i/rate.
//
// This is DESIGN.md decision 2 made concrete, and the one place in the repo
// where state is deliberately shared on the hot path. Decision 5 keeps
// histograms per-thread because a shared one turns the modal counter into a
// contended cache line — but a schedule cannot be per-thread. If each thread
// had its own, N threads at rate R would offer N*R.
//
// The cost is one atomic fetch_add per request against a syscall pair costing
// microseconds, and it is one cache line rather than 3,808 counters. Measured
// in E3 rather than assumed.
class Schedule {
public:
    Schedule(Rate rate, MonotonicClock::Instant start) : rate_(rate), start_(start) {}

    struct Slot {
        std::uint64_t index = 0;
        MonotonicClock::Instant due;
    };

    // Claim the next request. Lock-free, and the only mutation any thread makes
    // to shared state.
    //
    // `due` is what latency is measured FROM, not the moment the bytes go out.
    // A request the rig could not send on schedule accrues latency while it
    // waits, which is the entire difference between this and closed-loop: a
    // stalled closed-loop client stops sending and never times the requests it
    // failed to make.
    Slot claim() {
        const std::uint64_t index = next_.fetch_add(1, std::memory_order_relaxed);
        return {index, start_ + rate_.due_at(index)};
    }

    // How many slots have been handed out. Read after the run, so relaxed is
    // enough — the join is the synchronisation.
    std::uint64_t claimed() const { return next_.load(std::memory_order_relaxed); }

    Rate rate() const { return rate_; }
    MonotonicClock::Instant start() const { return start_; }

private:
    Rate rate_;
    MonotonicClock::Instant start_;
    std::atomic<std::uint64_t> next_{0};
};

}  // namespace dariyanaap
