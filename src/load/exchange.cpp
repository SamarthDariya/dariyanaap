#include "load/exchange.hpp"

#include "core/clock.hpp"
#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

Exchange perform_request(Socket& socket, const Protocol& protocol,
                         vector<char>& read_buffer, vector<char>& received) {
    Exchange result;
    received.clear();

    // The clock is read twice per request, explicitly, rather than once with a
    // lap() — see core/clock.hpp. A single reading would fold the rig's own
    // loop overhead into the reported latency.
    const MonotonicClock::Instant sent = MonotonicClock::now();

    try {
        socket.write_all(protocol.request());
    } catch (const TimedOut&) {
        // A write timeout has left an unknown prefix on the wire, so the
        // connection is finished either way. Counted as a timeout rather than
        // a write failure, because what happened is that the target stopped
        // reading — which is a hang, not a refusal.
        result.outcome = Outcome::TimedOut;
        result.elapsed = MonotonicClock::since(sent);
        return result;
    } catch (const IoError&) {
        result.outcome = Outcome::WriteFailed;
        return result;
    }

    for (;;) {
        size_t got = 0;
        try {
            got = socket.read_some({read_buffer.data(), read_buffer.size()});
        } catch (const TimedOut&) {
            result.outcome = Outcome::TimedOut;
            result.elapsed = MonotonicClock::since(sent);
            return result;
        } catch (const IoError&) {
            result.outcome = Outcome::ReadFailed;
            return result;
        }

        if (got == 0) {
            // A clean close before a whole reply. The socket layer reports this
            // as zero bytes rather than an error, because whether a short
            // response is a failure is a protocol question (chunk 2.3) — and
            // here, mid-response, the answer is yes.
            result.outcome = Outcome::ReadFailed;
            return result;
        }

        received.insert(received.end(), read_buffer.begin(),
                        read_buffer.begin() + static_cast<ptrdiff_t>(got));

        const ResponseState state = protocol.consume({received.data(), received.size()});
        if (state == ResponseState::NeedMore) {
            continue;
        }
        if (state == ResponseState::Malformed) {
            result.outcome = Outcome::ProtocolError;
            return result;
        }

        // Complete. Stop the clock before asking whether the target said yes,
        // so parsing a status line is not charged to the target.
        result.elapsed = MonotonicClock::between(sent, MonotonicClock::now());
        result.connection_usable = true;
        result.outcome = protocol.succeeded({received.data(), received.size()})
                             ? Outcome::Succeeded
                             : Outcome::Rejected;
        return result;
    }
}

}  // namespace dariyanaap
