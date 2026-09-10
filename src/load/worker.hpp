#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "core/address.hpp"
#include "core/clock.hpp"
#include "core/units.hpp"
#include "load/exchange.hpp"
#include "load/protocol.hpp"
#include "load/run_result.hpp"
#include "stats/histogram.hpp"

namespace dariyanaap {

struct WorkerConfig {
    Millis connect_timeout{1000};
    Millis read_timeout{1000};
    Millis write_timeout{1000};

    // How long to wait after a FAILED connect before trying again.
    //
    // Rig hygiene, not a retry policy. A refused connect on loopback returns
    // in microseconds, so retrying immediately turns a down target into a busy
    // loop that burns a core and inflates the connect counter into the
    // millions — noise that would also distort every other measurement on the
    // machine. It never applies to a request, only to a connection.
    Millis reconnect_delay{100};

    std::size_t read_buffer_bytes{16 * 1024};
};

// One connection, driven closed-loop: send, wait for the reply, send again.
//
// One of these per thread, owning its own Histogram, merged after the join
// (DESIGN.md decision 5). Nothing here is atomic and nothing locks.
//
// ON LOSING A CONNECTION — this answers the open question in DESIGN.md, which
// leaned the other way:
//
// The worker RECONNECTS, and never retries a request. Those are different
// things, and the open question conflated them. Retrying a request is unit 9's
// entire subject, and a rig with its own retry policy would contaminate it —
// so requests are never retried, and a failed one is counted and abandoned.
// But replacing a dead CONNECTION is not a policy, it is what keeps offered
// concurrency at the number the operator configured. Letting connections die
// unreplaced means a run configured for 500 connections quietly measures 300,
// then 80, and reports the result as though 500 were offered throughout.
// Silent decay of the independent variable is worse than either option the
// open question listed.
//
// The cost is visible rather than hidden: connections_opened() reports the
// churn, so a run where 500 connections became 50,000 opens says so.
class ConnectionWorker {
public:
    ConnectionWorker(const Protocol& protocol, std::vector<SocketAddress> addresses,
                     WorkerConfig config);

    // Work until `deadline`. Nothing that STARTS before `record_from` is
    // counted at all — not in the histogram, and not in the error counters
    // either. Warm-up has to mean one thing: if warm-up requests were counted
    // as attempts but excluded from the histogram, RunResult::consistent()
    // would report every run as having lost requests.
    void run(MonotonicClock::Instant record_from, MonotonicClock::Instant deadline);

    const Histogram& histogram() const { return histogram_; }
    const ErrorCounts& errors() const { return errors_; }
    std::uint64_t attempted() const { return attempted_; }
    std::uint64_t connections_opened() const { return connections_opened_; }

private:
    const Protocol& protocol_;
    std::vector<SocketAddress> addresses_;
    WorkerConfig config_;

    // Allocated once, reused by every request on every connection this worker
    // opens. Per-request allocation would be measured as target latency.
    std::vector<char> read_buffer_;
    std::vector<char> received_;

    Histogram histogram_;
    ErrorCounts errors_;
    std::uint64_t attempted_ = 0;
    std::uint64_t connections_opened_ = 0;
};

}  // namespace dariyanaap
