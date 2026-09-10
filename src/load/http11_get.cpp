#include "load/http11_get.hpp"

#include <cassert>
#include <optional>
#include <string_view>

#include "load/http.hpp"

using namespace std;

namespace dariyanaap {
namespace {

// Statuses defined to carry no body, so a response with no Content-Length is
// complete at the end of its headers rather than unframeable. Without this a
// target legitimately answering 204 would be counted as a protocol error,
// which is the misattribution decision 6 exists to prevent.
bool carries_no_body(int status) {
    return status == 204 || status == 304 || (status >= 100 && status < 200);
}

}  // namespace

Http11Get::Http11Get(const string& host, const string& path)
    : request_(http::build_get(host, path)) {}

span<const char> Http11Get::request() const {
    return {request_.data(), request_.size()};
}

ResponseState Http11Get::consume(span<const char> response) const {
    const string_view text(response.data(), response.size());

    const size_t blank = text.find("\r\n\r\n");
    if (blank == string_view::npos) {
        return text.size() > kMaxHeaderBytes ? ResponseState::Malformed
                                             : ResponseState::NeedMore;
    }

    const string_view headers = text.substr(0, blank);
    const size_t body_begins = blank + 4;

    if (http::header_value(headers, "transfer-encoding").has_value()) {
        // Named explicitly rather than falling through as "no Content-Length",
        // so the diagnosis says "this target uses chunked encoding, which is
        // out of scope" instead of "unframeable response".
        return ResponseState::Malformed;
    }

    const optional<uint64_t> declared = http::content_length(headers);
    if (!declared) {
        const optional<int> status = http::status_code(response);
        if (status && carries_no_body(*status)) {
            return text.size() == body_begins ? ResponseState::Complete
                                              : ResponseState::Malformed;
        }
        // A body we cannot frame. Guessing where it ends is what produces
        // silently wrong numbers, so this is a protocol error.
        return ResponseState::Malformed;
    }

    const size_t total = body_begins + static_cast<size_t>(*declared);
    if (text.size() < total) {
        return ResponseState::NeedMore;
    }
    if (text.size() == total) {
        return ResponseState::Complete;
    }
    // Past the declared end: the stream holds part of a second response, so
    // reply N's timing would be attributed to request N+1.
    return ResponseState::Malformed;
}

bool Http11Get::succeeded(span<const char> response) const {
    assert(consume(response) == ResponseState::Complete &&
           "succeeded() asked about a response that is not complete");

    const optional<int> status = http::status_code(response);
    return status.has_value() && *status >= 200 && *status < 300;
}

}  // namespace dariyanaap
