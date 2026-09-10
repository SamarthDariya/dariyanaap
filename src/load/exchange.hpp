#pragma once

#include <vector>

#include "core/socket.hpp"
#include "core/units.hpp"
#include "load/protocol.hpp"

namespace dariyanaap {

// What one request/response exchange did.
enum class Outcome {
    Succeeded,      // a whole reply, and the target said yes
    Rejected,       // a whole, well-formed reply saying no (non-2xx)
    TimedOut,       // nothing came back in time
    ProtocolError,  // a reply arrived that cannot be a reply
    ReadFailed,     // the stream broke before a whole reply
    WriteFailed,    // could not send
};

struct Exchange {
    Outcome outcome = Outcome::WriteFailed;

    // How long the request took. Meaningful for Succeeded, Rejected and
    // TimedOut — the three outcomes that produce a duration (chunk 2.8).
    //
    // For a timeout this is the ACTUAL elapsed time, not the configured
    // timeout. It is necessarily at least the timeout, so it never
    // under-reports, and it is the true answer to "how long did this request
    // take before we gave up".
    Nanos elapsed{0};

    // Whether the connection can carry another request.
    //
    // False after a timeout, because an unread reply may still be in flight
    // and the next request would read it — reporting reply N's timing against
    // request N+1. False after a protocol error for the same reason. Getting
    // this wrong produces plausible numbers from a desynchronised stream,
    // which is worse than an error.
    bool connection_usable = false;
};

// Send one request and read one reply.
//
// The buffers are the caller's and are reused across every request on the
// connection: allocating per request would put the allocator on the measured
// path and report it as the target's latency.
//
// Never throws. Every failure is an Outcome, because the caller's job is to
// count by kind (decision 6) and an exception would have to be translated
// back into exactly this enum anyway.
Exchange perform_request(Socket& socket, const Protocol& protocol,
                         std::vector<char>& read_buffer,
                         std::vector<char>& received);

}  // namespace dariyanaap
