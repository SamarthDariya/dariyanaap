#include "core/endpoint.hpp"

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

Endpoint::Endpoint(string host, uint16_t port) : host_(move(host)), port_(port) {
    if (host_.empty()) {
        throw InvalidEndpoint("endpoint host must not be empty");
    }
    if (port_ == 0) {
        // Port 0 means "let the kernel pick" when binding, and means nothing
        // at all as a destination. Refusing it here turns a run that would
        // have died at connect() into an argument error raised before any
        // measurement starts.
        throw InvalidEndpoint("endpoint port must not be zero, for host " + host_);
    }
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
