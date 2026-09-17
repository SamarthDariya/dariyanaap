#include "load/open_loop.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include "core/errors.hpp"
#include "core/socket.hpp"
#include "load/exchange.hpp"
#include "stats/summary.hpp"

using namespace std;

namespace dariyanaap {
namespace {

// One connection, driven by the shared schedule rather than by replies.
//
// The difference from ConnectionWorker is two lines long and is the whole
// lesson of unit 0: the latency recorded is measured from the slot's DUE time,
// not from the moment the request was written. A request that waited because
// every connection was busy carries that wait in its latency — which is
// exactly the request a closed-loop client would never have sent, and would
// therefore never have timed.
struct OpenLoopWorker {
    const Protocol& protocol;
    vector<SocketAddress> addresses;
    WorkerConfig config;
    Schedule& schedule;

    vector<char> read_buffer;
    vector<char> received;
    Histogram histogram;
    Histogram lag;
    ErrorCounts errors;
    uint64_t attempted = 0;
    uint64_t connections_opened = 0;

    OpenLoopWorker(const Protocol& p, vector<SocketAddress> a, WorkerConfig c, Schedule& s)
        : protocol(p), addresses(std::move(a)), config(c), schedule(s),
          read_buffer(c.read_buffer_bytes) {
        received.reserve(c.read_buffer_bytes);
    }

    void run(MonotonicClock::Instant record_from, MonotonicClock::Instant deadline) {
        optional<Socket> socket;

        for (;;) {
            if (MonotonicClock::now() >= deadline) {
                return;
            }
            if (!socket) {
                try {
                    socket = Socket::connect_any(addresses, config.connect_timeout);
                    socket->set_timeouts(config.read_timeout, config.write_timeout);
                    ++connections_opened;
                } catch (const IoError&) {
                    if (MonotonicClock::now() >= record_from) {
                        ++attempted;
                        ++errors.connect;
                    }
                    this_thread::sleep_for(config.reconnect_delay);
                    continue;
                }
            }

            const Schedule::Slot slot = schedule.claim();
            if (slot.due >= deadline) {
                // The schedule has run past the end of the run. Claiming it was
                // still correct — slots_claimed has to reflect the work the
                // rate demanded — but sending it would measure past the window.
                return;
            }
            // Sleep only if we are early. If we are late, send at once and let
            // the lateness show up in the latency, which is where it belongs.
            this_thread::sleep_until(slot.due);

            const MonotonicClock::Instant sending = MonotonicClock::now();
            const Exchange result = perform_request(*socket, protocol, read_buffer, received);
            const bool measuring = slot.due >= record_from;

            if (measuring) {
                ++attempted;
                lag.record(max(Nanos(0), MonotonicClock::between(slot.due, sending)));

                // The one line that separates this file from worker.cpp: the
                // latency starts at slot.due, not at `sending`.
                const Nanos from_due =
                    MonotonicClock::between(slot.due, sending) + result.elapsed;

                switch (result.outcome) {
                    case Outcome::Succeeded:
                        histogram.record(from_due);
                        break;
                    case Outcome::Rejected:
                        ++errors.rejected;
                        histogram.record(from_due);
                        break;
                    case Outcome::TimedOut:
                        ++errors.timeout;
                        histogram.record(from_due);
                        break;
                    case Outcome::ProtocolError: ++errors.protocol; break;
                    case Outcome::ReadFailed: ++errors.read; break;
                    case Outcome::WriteFailed: ++errors.write; break;
                }
            }

            if (!result.connection_usable) {
                socket.reset();
            }
        }
    }
};

}  // namespace

OpenLoopRun run_open_loop(const Protocol& protocol, const OpenLoopPlan& plan) {
    const vector<SocketAddress> addresses = resolve(plan.target);

    const MonotonicClock::Instant started = MonotonicClock::now();
    const MonotonicClock::Instant record_from = started + plan.warmup;
    const MonotonicClock::Instant deadline = record_from + plan.duration;

    // One schedule for the whole run, starting at the warm-up boundary so slot
    // zero is the first measured request.
    Schedule schedule(plan.rate, record_from);

    vector<unique_ptr<OpenLoopWorker>> workers;
    workers.reserve(plan.connections);
    vector<thread> threads;
    threads.reserve(plan.connections);

    OpenLoopRun run;
    run.connections_requested = plan.connections;

    for (size_t i = 0; i < plan.connections; ++i) {
        workers.push_back(make_unique<OpenLoopWorker>(protocol, addresses, plan.worker,
                                                       schedule));
        try {
            threads.emplace_back([worker = workers.back().get(), record_from, deadline] {
                worker->run(record_from, deadline);
            });
        } catch (const system_error&) {
            workers.pop_back();
            break;
        }
    }
    run.connections_started = threads.size();

    this_thread::sleep_until(record_from);
    run.clock_resolution = MonotonicClock::measured_resolution();

    for (thread& worker : threads) {
        worker.join();
    }

    for (const unique_ptr<OpenLoopWorker>& worker : workers) {
        run.histogram.merge(worker->histogram);
        run.schedule_lag.merge(worker->lag);
        run.result.errors.merge(worker->errors);
        run.result.attempted += worker->attempted;
        run.connections_opened += worker->connections_opened;
    }

    run.result.duration = plan.duration;
    run.result.latency = Summary::of(run.histogram, plan.duration);
    run.rate = plan.rate;
    run.slots_claimed = schedule.claimed();
    run.slots_due = static_cast<uint64_t>(
        plan.rate.rps() * static_cast<double>(plan.duration.count()) / 1000.0);
    return run;
}

}  // namespace dariyanaap
