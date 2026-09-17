#pragma once

#include <cstddef>
#include <cstdint>

#include "core/endpoint.hpp"
#include "core/units.hpp"
#include "load/protocol.hpp"
#include "load/run_result.hpp"
#include "load/worker.hpp"

namespace dariyanaap {

struct ClosedLoopPlan {
    Endpoint target{"127.0.0.1", 8080};

    // Connections held open, each sending one request at a time. This is the
    // independent variable of E2's sweep.
    std::size_t connections = 1;

    // How long to measure for, AFTER warm-up. Total wall clock is
    // warmup + duration.
    Millis duration{10'000};

    // Discarded window at the start. Not a hidden constant: it is printed in
    // the CSV header, because unit 2 is specifically about a backend restarting
    // with a cold cache and getting slammed, and there the warm-up is the
    // experiment rather than noise (DESIGN.md decision 8).
    Millis warmup{0};

    WorkerConfig worker;
};

// One closed-loop run, and everything a reader needs to distrust it.
struct ClosedLoopRun {
    RunResult result;

    std::size_t connections_requested = 0;

    // How many threads actually started. Fewer than requested means the RIG
    // hit a limit, not the target — and a run that offered less load than
    // asked for must say so rather than have its throughput compared against
    // runs that did not. E2's whole purpose is finding this number.
    std::size_t connections_started = 0;

    // TCP connects across the whole run, which exceeds connections_started
    // whenever connections were replaced (chunk 2.9b).
    std::uint64_t connections_opened = 0;

    // Measured at the END of warm-up, per BREAK.md E0: at process start the
    // core has not clocked up and the figure is 2.5x too pessimistic.
    //
    // Measured while the workers are running, so it includes scheduler
    // contention at the configured concurrency. That is deliberate — decision
    // 7 wants the floor on any latency THIS RUN could report, and during a
    // 500-connection run that floor genuinely includes contention. Expect it
    // to rise with connections, and expect a 1-connection run to sit near the
    // hardware tick.
    Nanos clock_resolution{0};
};

// Resolve once, spawn `connections` workers, wait, merge.
//
// Resolution happens here rather than per connection (chunk 2.2a): 500 threads
// each calling getaddrinfo would hammer DNS, and DNS failures would land in the
// run's connect-error count looking exactly like the target refusing us.
ClosedLoopRun run_closed_loop(const Protocol& protocol, const ClosedLoopPlan& plan);

}  // namespace dariyanaap
