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
    // Kept as the total, and split by cause into the two histograms below,
    // because they are different findings and this field alone conflated them.
    // See kept_up().
    Histogram schedule_lag;

    // The part of the lag spent waiting for a connection to come free.
    //
    // Every connection carries one request at a time (exchange.cpp is
    // synchronous and there is no pipelining), so a target that holds a
    // connection for 20ms caps what `connections` can offer at
    // `connections / 0.02s`, however high --rate is set. Past that point slots
    // come due while every worker is still mid-request, and they wait.
    //
    // That wait is real, it is in the reported latency, and it is NOT the rig
    // failing to keep up. It is the answer to "how many connections does this
    // target need before it can be offered that rate" — unit 1's question.
    //
    // A reconnect lands here too: time spent without a usable socket is time
    // no slot could be carried.
    Histogram connection_wait;

    // The part that is the rig's own doing: the worker was idle, waiting for a
    // slot that had not come due yet, and still sent late. Scheduler
    // overshoot, or a machine too busy running the rig to run it on time.
    //
    // This is the one with teeth. Recorded separately rather than derived at
    // the end, because percentiles do not subtract.
    Histogram rig_lag;

    // Slots the schedule handed out, against what the rate demanded for the
    // measured window. Reported as information, not as the verdict — see below.
    std::uint64_t slots_claimed = 0;
    std::uint64_t slots_due = 0;

    Rate rate = Rate::per_second(1.0);

    // Did the SENDER stay on schedule?
    //
    // Judged on the median rig_lag, and this question has now been answered
    // wrongly twice.
    //
    // First version: slots_claimed against slots_due. That declared a 60,000
    // rps run void while an 80,000 rps run passed, against the same stalling
    // target — because the slot count fails whenever a stall merely overlaps
    // the end of the measured window, with no fault of the rig's. The two
    // runs' lag told the truth plainly: p50 of 32µs at 60k, 67ms at 98k.
    //
    // Second version: the median of the TOTAL lag. That is the misattribution
    // unit 1 found before it had written a line of its own code. A worker
    // blocked in a 20ms request cannot claim its next slot on time, so a slow
    // target produces schedule lag identical in shape to a saturated rig — and
    // the rig blamed itself, printed THIS RUN IS VOID and exited 1 on exactly
    // the runs unit 1 exists to produce. It was keeping up perfectly. It was
    // waiting for the target.
    //
    // So the question is whether the sender was PERSISTENTLY behind for a
    // reason of its own, which is rig_lag and nothing else. The threshold is
    // ten schedule intervals: at 80,000 rps that is 125µs against a measured
    // median of 38µs, and at 98,000 rps it is 102µs against 67,633µs.
    //
    // Connection starvation is not thereby silenced — it is connection_wait,
    // plus slots_claimed falling short of slots_due, and the CLI reports both.
    // It is just not a verdict on the rig.
    bool kept_up() const {
        if (rig_lag.count() == 0) {
            return true;  // nothing was scheduled, so nothing fell behind
        }
        const std::optional<Nanos> median = rig_lag.percentile(50.0);
        return median.has_value() && *median < rate.interval() * 10;
    }

    // Was the connection pool, rather than --rate, what limited this run?
    //
    // The run is valid and its latencies are honest — the wait is in them. But
    // the rate actually offered was below the rate configured, and a report
    // that quotes the configured one is describing an experiment that did not
    // happen. Same "persistently behind" test, same ten-interval threshold,
    // asked of the other half of the lag.
    bool connections_saturated() const {
        if (connection_wait.count() == 0) {
            return false;
        }
        const std::optional<Nanos> median = connection_wait.percentile(50.0);
        return median.has_value() && *median >= rate.interval() * 10;
    }
};

// Resolve once, spawn `connections` workers sharing one schedule, wait, merge.
OpenLoopRun run_open_loop(const Protocol& protocol, const OpenLoopPlan& plan);

}  // namespace dariyanaap
