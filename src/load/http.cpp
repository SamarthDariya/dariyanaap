#include "load/http.hpp"

#include "core/errors.hpp"
#include "core/version.hpp"

using namespace std;

namespace dariyanaap::http {
namespace {

// A CR or LF anywhere in a host or path ends the request line or a header
// early, so the target receives something other than what was asked for and
// its complaint is attributed to the target. Refusing here turns a silent
// mis-measurement into an argument error before the run starts.
void reject_crlf(const string& value, const char* field) {
    if (value.find('\r') != string::npos || value.find('\n') != string::npos) {
        throw UsageError(string(field) + " must not contain CR or LF");
    }
}

}  // namespace

string build_get(const string& host, const string& path) {
    if (host.empty()) {
        throw UsageError("HTTP host must not be empty; HTTP/1.1 requires a Host header");
    }
    if (path.empty() || path.front() != '/') {
        throw UsageError("HTTP path must start with '/', got \"" + path + "\"");
    }
    reject_crlf(host, "HTTP host");
    reject_crlf(path, "HTTP path");

    // Built once per run, so readability wins over avoiding the allocations.
    // The User-Agent is here so a target's access log says which tool hit it —
    // useful when a unit's own service is logging and the run looks wrong.
    string request;
    request += "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + "\r\n";
    request += "User-Agent: " + string(version()) + "\r\n";
    request += "Accept: */*\r\n";
    request += "\r\n";
    return request;
}

optional<int> status_code(span<const char> response) {
    // "HTTP/1.1 200" is 12 bytes, the shortest prefix that can carry a code.
    constexpr size_t kShortest = 12;
    if (response.size() < kShortest) {
        return nullopt;
    }
    const string_view head(response.data(), response.size());
    if (!head.starts_with("HTTP/1.1 ") && !head.starts_with("HTTP/1.0 ")) {
        // Not HTTP at all. Reported the same as "not yet" — see the header:
        // the caller separates the two by whether the response is complete.
        return nullopt;
    }

    int code = 0;
    for (size_t i = 9; i < 12; ++i) {
        const char digit = head[i];
        if (digit < '0' || digit > '9') {
            return nullopt;
        }
        code = code * 10 + (digit - '0');
    }
    // A fourth digit means the status line is not a status line.
    if (response.size() > 12 && head[12] >= '0' && head[12] <= '9') {
        return nullopt;
    }
    return code;
}

}  // namespace dariyanaap::http
