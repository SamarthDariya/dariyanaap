#pragma once

#include <cstddef>
#include <span>

namespace dariyanaap {

enum class ResponseState {
    NeedMore,   // a complete response has not arrived yet; read again
    Complete,   // exactly one response is present
    Malformed,  // the bytes cannot be a response — a protocol error, which
                // decision 6 counts separately from a read failure
};

// What the driver needs to know about a protocol, and nothing else.
//
// Three methods, because the runner has exactly three questions: what do I
// send, have I received a whole reply, and did the target say yes. Keeping
// completeness (`consume`) apart from success (`succeeded`) is decision 6 in
// the type system: an HTTP 503 is a well-formed complete response AND a
// failure, and a rig that conflated them could not tell "the backend returned
// 503 instantly" from "the backend hung until we gave up".
//
// Every method is const, and the object is shared by every connection in a
// run rather than copied per thread. That costs a re-scan of the accumulated
// buffer on each read — quadratic in response size, which is affordable only
// because the responses in this track are hundreds of bytes. A protocol that
// ever needs megabyte responses becomes stateful and per-connection, and this
// comment is the warning that such a change is a redesign rather than an edit.
//
// Dispatch is virtual. Two virtual calls per request cost a handful of
// nanoseconds against a syscall pair costing microseconds, and the CLI has to
// pick a protocol from a flag at runtime anyway — a template would only move
// the dispatch, not remove it. E2 measures the rig's ceiling with this in
// place, so the cost is in the published number rather than argued about.
class Protocol {
public:
    virtual ~Protocol() = default;

    // The bytes of one request. Built once and returned by reference, so the
    // send path does no work per request beyond the write itself.
    virtual std::span<const char> request() const = 0;

    // Everything received for the current response so far.
    virtual ResponseState consume(std::span<const char> response) const = 0;

    // Whether a Complete response means the target succeeded. Only meaningful
    // once consume() has returned Complete.
    virtual bool succeeded(std::span<const char> response) const = 0;
};

}  // namespace dariyanaap
