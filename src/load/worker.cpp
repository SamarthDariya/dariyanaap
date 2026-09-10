#include "load/worker.hpp"

#include <optional>
#include <thread>
#include <utility>

#include "core/errors.hpp"
#include "core/socket.hpp"

using namespace std;

namespace dariyanaap {

ConnectionWorker::ConnectionWorker(const Protocol& protocol,
                                   vector<SocketAddress> addresses,
                                   WorkerConfig config)
    : protocol_(protocol),
      addresses_(std::move(addresses)),
      config_(config),
      read_buffer_(config.read_buffer_bytes),
      received_() {
    received_.reserve(config.read_buffer_bytes);
}

void ConnectionWorker::run(MonotonicClock::Instant record_from,
                           MonotonicClock::Instant deadline) {
    optional<Socket> socket;

    for (;;) {
        // One clock read serves both questions — is the run over, and are we
        // past warm-up — so the loop costs one reading per request rather than
        // two. Measuring is decided by when the request STARTS: a request
        // whose latency includes the last of the cold-cache period does not
        // belong in the distribution.
        const MonotonicClock::Instant now = MonotonicClock::now();
        if (now >= deadline) {
            return;
        }
        const bool measuring = now >= record_from;

        if (!socket) {
            try {
                socket = Socket::connect_any(addresses_, config_.connect_timeout);
                socket->set_timeouts(config_.read_timeout, config_.write_timeout);
                ++connections_opened_;
            } catch (const IoError&) {
                if (measuring) {
                    // Counted as an attempt because it was one: a turn of this
                    // loop that failed before a request could be sent. Without
                    // it, RunResult::consistent() would not balance.
                    ++attempted_;
                    ++errors_.connect;
                }
                this_thread::sleep_for(config_.reconnect_delay);
                continue;
            }
        }

        const Exchange result =
            perform_request(*socket, protocol_, read_buffer_, received_);

        if (measuring) {
            ++attempted_;
            switch (result.outcome) {
                case Outcome::Succeeded:
                    histogram_.record(result.elapsed);
                    break;
                case Outcome::Rejected:
                    // In the histogram AND in a counter: it took real time and
                    // it was a failure (chunk 2.8).
                    ++errors_.rejected;
                    histogram_.record(result.elapsed);
                    break;
                case Outcome::TimedOut:
                    // Recorded, never dropped. Dropping the slowest requests
                    // of a run is coordinated omission.
                    ++errors_.timeout;
                    histogram_.record(result.elapsed);
                    break;
                case Outcome::ProtocolError:
                    ++errors_.protocol;
                    break;
                case Outcome::ReadFailed:
                    ++errors_.read;
                    break;
                case Outcome::WriteFailed:
                    ++errors_.write;
                    break;
            }
        }

        if (!result.connection_usable) {
            socket.reset();
        }
    }
}

}  // namespace dariyanaap
