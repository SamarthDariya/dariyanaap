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

namespace {

bool equal_ignoring_case(string_view a, string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        // Only ASCII letters differ by this bit, and header names are ASCII.
        // tolower() would drag in the locale, which can make "I" lowercase to
        // a dotless i in a Turkish locale and break header matching.
        const char left = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        const char right = (b[i] >= 'A' && b[i] <= 'Z') ? static_cast<char>(b[i] + 32) : b[i];
        if (left != right) {
            return false;
        }
    }
    return true;
}

string_view trim(string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

}  // namespace

optional<string_view> header_value(string_view headers, string_view name) {
    // Walk lines rather than searching for "name:" anywhere: a substring
    // search would match "X-Not-Content-Length" and would also match text
    // inside another header's value.
    size_t line_begins = headers.find("\r\n");
    if (line_begins == string_view::npos) {
        return nullopt;  // status line only, no headers
    }
    line_begins += 2;

    while (line_begins < headers.size()) {
        size_t line_ends = headers.find("\r\n", line_begins);
        if (line_ends == string_view::npos) {
            line_ends = headers.size();
        }
        const string_view line = headers.substr(line_begins, line_ends - line_begins);
        const size_t colon = line.find(':');
        if (colon != string_view::npos &&
            equal_ignoring_case(line.substr(0, colon), name)) {
            return trim(line.substr(colon + 1));
        }
        line_begins = line_ends + 2;
    }
    return nullopt;
}

optional<uint64_t> content_length(string_view headers) {
    const optional<string_view> value = header_value(headers, "content-length");
    if (!value || value->empty()) {
        return nullopt;
    }
    uint64_t length = 0;
    for (const char digit : *value) {
        if (digit < '0' || digit > '9') {
            // No signs, no whitespace inside, no hex. A Content-Length we
            // cannot read is a body we cannot frame.
            return nullopt;
        }
        if (length > (UINT64_MAX - static_cast<uint64_t>(digit - '0')) / 10) {
            return nullopt;  // would overflow; not a real body length
        }
        length = length * 10 + static_cast<uint64_t>(digit - '0');
    }
    return length;
}

}  // namespace dariyanaap::http
