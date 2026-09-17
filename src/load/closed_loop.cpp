#include "load/closed_loop.hpp"

#include <memory>
#include <thread>
#include <vector>

#include "core/address.hpp"
#include "core/clock.hpp"
#include "stats/summary.hpp"

using namespace std;

namespace dariyanaap {

ClosedLoopRun run_closed_loop(const Protocol& protocol, const ClosedLoopPlan& plan) {
    const vector<SocketAddress> addresses = resolve(plan.target);

    const MonotonicClock::Instant started = MonotonicClock::now();
    const MonotonicClock::Instant record_from = started + plan.warmup;
    const MonotonicClock::Instant deadline = record_from + plan.duration;

    // unique_ptr rather than a vector of workers by value: a worker holds a
    // 30KB histogram and a reference to the protocol, and a reallocation
    // mid-spawn would move objects the running threads are pointing at.
    vector<unique_ptr<ConnectionWorker>> workers;
    workers.reserve(plan.connections);
    vector<thread> threads;
    threads.reserve(plan.connections);

    ClosedLoopRun run;
    run.connections_requested = plan.connections;

    for (size_t i = 0; i < plan.connections; ++i) {
        workers.push_back(
            make_unique<ConnectionWorker>(protocol, addresses, plan.worker));
        try {
            threads.emplace_back([worker = workers.back().get(), record_from, deadline] {
                worker->run(record_from, deadline);
            });
        } catch (const system_error&) {
            // Out of threads. Not an error to abort on — it is the answer E2 is
            // looking for. Drop the worker we could not drive and carry on with
            // however many started, reporting the shortfall.
            workers.pop_back();
            break;
        }
    }
    run.connections_started = threads.size();

    // The one place measured_resolution() is called, at the warm-up boundary,
    // with the workers already running.
    this_thread::sleep_until(record_from);
    run.clock_resolution = MonotonicClock::measured_resolution();

    for (thread& worker : threads) {
        worker.join();
    }

    // Merged on one thread after every worker has stopped. Nothing above is
    // atomic and nothing locks, which is the claim decision 5 rests on and the
    // reason TSan is wired into this repo.
    Histogram merged;
    for (const unique_ptr<ConnectionWorker>& worker : workers) {
        merged.merge(worker->histogram());
        run.result.errors.merge(worker->errors());
        run.result.attempted += worker->attempted();
        run.connections_opened += worker->connections_opened();
    }

    run.result.duration = plan.duration;
    run.result.latency = Summary::of(merged, plan.duration);
    return run;
}

}  // namespace dariyanaap
