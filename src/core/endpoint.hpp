#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dariyanaap {

// A host and port to point load at, or to bind a target to.
//
// Deliberately not called Target: a target is an endpoint *plus* a protocol
// and a request body, and that composite arrives at M2. Keeping the address
// separate lets the fault library — which must name peers for
// fault::partition (decision 10) — depend on this without pulling in HTTP.
//
// A class rather than a struct of public fields, and no default constructor,
// for the same reason Rate has none (units.hpp): a default-constructed
// Endpoint would have an empty host and port 0, which is not an address. Both
// constructors validate, so an Endpoint that exists is one that could be
// connected to. Absence is std::optional<Endpoint>.
class Endpoint {
public:
    // Throws InvalidEndpoint if the host is empty or the port is zero.
    Endpoint(std::string host, std::uint16_t port);

    // Accepts "host:port", "1.2.3.4:8080", and bracketed IPv6 "[::1]:8080".
    // Strict: this parses one flag, once, before a run starts, so there is no
    // cost to rejecting input and a real cost to guessing. A rig that reads
    // "localhost" as port 0 produces a run describing something other than
    // what the operator asked for.
    static Endpoint parse(std::string_view text);

    const std::string& host() const { return host_; }
    std::uint16_t port() const { return port_; }

    bool is_ipv6() const;

    // Round-trips through parse(), re-bracketing IPv6. This is the form
    // stamped into the CSV header, so a run is reproducible from its own
    // output rather than from shell history.
    std::string str() const;

    friend bool operator==(const Endpoint& a, const Endpoint& b) {
        return a.port_ == b.port_ && a.host_ == b.host_;
    }

private:
    std::string host_;
    std::uint16_t port_;
};

}  // namespace dariyanaap
