#include "load/raw_echo.hpp"

#include <cassert>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

RawEcho::RawEcho(size_t size) {
    if (size == 0) {
        // A zero-byte request would make consume() report Complete on an empty
        // buffer, so every read would look like a finished response and the
        // run would measure nothing at full speed.
        throw UsageError("raw echo payload size must be at least 1 byte");
    }
    // A recognisable, non-uniform pattern rather than zeros, so a target that
    // replies with a zeroed buffer of the right length is visible in a dump
    // instead of passing as a correct echo.
    payload_.resize(size);
    for (size_t i = 0; i < size; ++i) {
        payload_[i] = static_cast<char>('a' + (i % 26));
    }
}

span<const char> RawEcho::request() const {
    return {payload_.data(), payload_.size()};
}

ResponseState RawEcho::consume(span<const char> response) const {
    if (response.size() < payload_.size()) {
        return ResponseState::NeedMore;
    }
    if (response.size() == payload_.size()) {
        return ResponseState::Complete;
    }
    // More bytes than were asked for. Not a long response — a desynchronised
    // stream, which means the target replied twice or the previous response
    // was mis-framed. Continuing would report timings for reply N against
    // request N+1, so it is a protocol error and the connection is finished.
    return ResponseState::Malformed;
}

// [[maybe_unused]] because the parameter is read only by the assert, and the
// measurement build defines NDEBUG. The sanitizer builds pass -UNDEBUG, so the
// check is live exactly where bugs are being hunted.
bool RawEcho::succeeded([[maybe_unused]] span<const char> response) const {
    assert(consume(response) == ResponseState::Complete &&
           "succeeded() asked about a response that is not complete");
    // Length was the check. See the class comment: content verification is the
    // test suite's job, not the measured path's.
    return true;
}

}  // namespace dariyanaap
