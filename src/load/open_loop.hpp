#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/address.hpp"
#include "core/endpoint.hpp"
#include "load/protocol.hpp"
#include "load/run_result.hpp"
#include "load/schedule.hpp"
#include "load/worker.hpp"
#include "stats/histogram.hpp"

namespace dariyanaap {

struct OpenLoopPlan {
    Endpoint target{"127.0.0.1", 8080};

    // Offered load. Unlike closed-loop, this is what the operator chooses and
    // the target's speed does not change it.
    Rate rate = Rate::per_second(1000.0);

    // Connections available to carry the schedule. Not the offered load — if
    // every connection is busy when a request comes due, the request waits,
    // and the wait is reported as latency because it is.
    std::size_t connections = 1;

    Millis duration{10'000};
    Millis warmup{0};
    WorkerConfig worker;
};

struct OpenLoopRun {
    RunResult result;
    Histogram histogram;

    std::size_t connections_requested = 0;
    std::size_t connections_started = 0;
    std::uint64_t connections_opened = 0;
    Nanos clock_resolution{0};

    // How far behind its own plan the sender fell, measured as
    // (actual send time - intended send time) over every request.
    //
    // DESIGN.md decision 7's second self-check, and it has teeth: if this is
    // large the rig could not keep up, the offered load was not what was
    // configured, and the run is void. Reporting the numbers anyway would be
    // publishing a rate that was never offered.
    Histogram schedule_lag;

    // Slots the schedule handed out, against what the rate demanded for the
    // measured window. Reported as information, not as the verdict — see below.
    std::uint64_t slots_claimed = 0;
    std::uint64_t slots_due = 0;

    Rate rate = Rate::per_second(1.0);

    // Did the sender stay on schedule?
    //
    // Judged on the MEDIAN lag, not on the slot count, and the first version
    // of this got it wrong. Comparing slots_claimed against slots_due declared
    // a 60,000 rps run void while an 80,000 rps run passed, against the same
    // stalling target — because the slot count fails whenever a stall merely
    // overlaps the end of the measured window, with no fault of the rig's. The
    // two runs' lag told the truth plainly: p50 of 32µs at 60k, 67ms at 98k.
    //
    // So the question is whether the sender was PERSISTENTLY behind, which is
    // a question about the middle of the distribution. The threshold is ten
    // schedule intervals: at 80,000 rps that is 125µs against a measured
    // median of 38µs, and at 98,000 rps it is 102µs against 67,633µs.
    //
    // A momentary shortfall at the window's edge is still visible in
    // slots_claimed for anyone who wants it.
    bool kept_up() const {
        if (schedule_lag.count() == 0) {
            return true;  // nothing was scheduled, so nothing fell behind
        }
        const std::optional<Nanos> median = schedule_lag.percentile(50.0);
        return median.has_value() && *median < rate.interval() * 10;
    }
};

// Resolve once, spawn `connections` workers sharing one schedule, wait, merge.
OpenLoopRun run_open_loop(const Protocol& protocol, const OpenLoopPlan& plan);

}  // namespace dariyanaap
