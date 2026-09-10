#pragma once

#include <cstddef>
#include <span>
#include <string>

#include "load/protocol.hpp"

namespace dariyanaap {

// GET one path over HTTP/1.1, framing responses by Content-Length.
//
// The protocol for units 1, 3, 7, 9, 11 and 12 — everything whose target is a
// service rather than a socket. RawEcho is for the rest, and is what E2
// calibrates with, because it makes the target's work as close to zero as a
// socket allows.
//
// Framing is Content-Length only. Chunked encoding is out of scope
// permanently (DESIGN.md decision 11), and a chunked response is reported as
// Malformed with that as the reason rather than silently mis-framed — a
// response framed at the wrong length desynchronises every request after it on
// the same connection, and produces wrong latencies with no error at all.
class Http11Get : public Protocol {
public:
    // Throws UsageError if the host or path cannot make a well-formed request.
    Http11Get(const std::string& host, const std::string& path);

    std::span<const char> request() const override;
    ResponseState consume(std::span<const char> response) const override;

    // 2xx. A 3xx is a complete, well-formed response and not a success:
    // decision 11 excludes redirects, so following one is not on the table,
    // and counting it as a success would report a target that never served the
    // request as having served it.
    bool succeeded(std::span<const char> response) const override;

    // A header block this large without a blank line means the target is not
    // going to send one. The read timeout already bounds the wait; this bounds
    // the memory, and turns a target babbling headers into a protocol error
    // rather than a growing buffer.
    static constexpr std::size_t kMaxHeaderBytes = 64 * 1024;

private:
    std::string request_;
};

}  // namespace dariyanaap
