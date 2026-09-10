#include "core/endpoint.hpp"

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {
namespace {

// [[noreturn]] so parse_port needs no unreachable return after a rejection.
[[noreturn]] void reject(string_view text, const string& why) {
    throw InvalidEndpoint("cannot parse endpoint \"" + string(text) + "\": " + why);
}

// Hand-rolled rather than stoi, which accepts "80abc", " 80" and "+80" — all
// three silently, returning 80 — and signals real failure by throwing
// std::invalid_argument or std::out_of_range rather than the type this file
// promises. Nothing about stoi is usable here.
//
// Note what is absent: no check for zero. That invariant lives in the
// constructor, so parse() gets it for free rather than enforcing it twice.
uint16_t parse_port(string_view text, string_view whole) {
    if (text.empty()) {
        reject(whole, "port is missing");
    }
    uint32_t port = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            reject(whole, "port must be decimal digits, found '" + string(1, c) + "'");
        }
        port = port * 10 + static_cast<uint32_t>(c - '0');
        if (port > 65535) {
            // Checked inside the loop, so "99999999999" is rejected rather
            // than overflowed into something plausible.
            reject(whole, "port must be at most 65535");
        }
    }
    return static_cast<uint16_t>(port);
}

}  // namespace

Endpoint::Endpoint(string host, uint16_t port) : host_(std::move(host)), port_(port) {
    if (host_.empty()) {
        throw InvalidEndpoint("endpoint host must not be empty");
    }
    if (host_.find('[') != string::npos || host_.find(']') != string::npos) {
        // parse() strips brackets and str() puts them back, so a host that
        // still contains one came from a caller who passed the textual form to
        // the constructor instead of to parse(). Left unchecked it produces
        // "[[::1]]:8080", which resolves to nothing — and the failure surfaces
        // at connect time as an unresolvable host rather than here.
        throw InvalidEndpoint("endpoint host must not contain brackets, got \"" +
                              host_ + "\" — use Endpoint::parse for \"[::1]:8080\" form");
    }
    if (port_ == 0) {
        // Port 0 means "let the kernel pick" when binding, and means nothing
        // at all as a destination. Refusing it here turns a run that would
        // have died at connect() into an argument error raised before any
        // measurement starts.
        throw InvalidEndpoint("endpoint port must not be zero, for host " + host_);
    }
}

Endpoint Endpoint::parse(string_view text) {
    if (text.empty()) {
        reject(text, "empty");
    }

    string_view host;
    string_view port;

    if (text.front() == '[') {
        // The brackets exist because the address is full of colons, so find
        // the closing one before looking for the host/port separator.
        const size_t close = text.find(']');
        if (close == string_view::npos) {
            reject(text, "unclosed '[' in IPv6 address");
        }
        host = text.substr(1, close - 1);
        const string_view rest = text.substr(close + 1);
        if (rest.empty() || rest.front() != ':') {
            reject(text, "bracketed IPv6 address must be followed by \":port\"");
        }
        port = rest.substr(1);
        if (host.find(':') == string_view::npos) {
            reject(text, "brackets are for IPv6 addresses, which contain ':'");
        }
    } else {
        // rfind, not find: the last colon is the separator. This branch then
        // refuses any host still holding a colon, which is what makes
        // is_ipv6()'s colon test sound.
        const size_t colon = text.rfind(':');
        if (colon == string_view::npos) {
            reject(text, "expected \"host:port\"");
        }
        host = text.substr(0, colon);
        port = text.substr(colon + 1);
        if (host.find(':') != string_view::npos) {
            // "::1:8080" — is 8080 the port, or the address's last group?
            // Both readings are defensible, so refuse rather than pick.
            reject(text, "IPv6 addresses must be bracketed, as \"[::1]:8080\"");
        }
    }

    for (const char c : host) {
        // Characters that mean this came from a URL, or from a shell that
        // handed over an unquoted argument. Accepting "http://localhost:8080"
        // would aim load at a host named "http:" instead of saying so.
        if (c == ' ' || c == '\t' || c == '/' || c == '\\' || c == '@' || c == '?') {
            reject(text, "host contains an invalid character '" + string(1, c) + "'");
        }
    }

    // Empty host and zero port are the constructor's invariants, checked there.
    return Endpoint(string(host), parse_port(port, text));
}

bool Endpoint::is_ipv6() const {
    // A colon cannot appear in a hostname or an IPv4 literal, so its presence
    // is the whole test. parse() is what upholds that: it rejects any
    // unbracketed host containing one.
    return host_.find(':') != string::npos;
}

string Endpoint::str() const {
    if (is_ipv6()) {
        return "[" + host_ + "]:" + to_string(port_);
    }
    return host_ + ":" + to_string(port_);
}

}  // namespace dariyanaap
