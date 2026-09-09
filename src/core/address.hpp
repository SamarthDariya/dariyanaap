#pragma once

#include <sys/socket.h>

#include <string>
#include <vector>

#include "core/endpoint.hpp"

namespace dariyanaap {

// One resolved address to connect to: a sockaddr plus its length, sized to
// hold either family.
//
// A plain value — copyable, unlike Socket — because it owns no resource. That
// is what lets one resolution be shared by every connection in a run.
class SocketAddress {
public:
    SocketAddress(const sockaddr* addr, socklen_t size);

    const sockaddr* addr() const;
    socklen_t size() const { return size_; }
    int family() const;

    // Numeric host and port, never a reverse lookup. Goes into error messages
    // and the CSV header, and a reverse DNS round trip inside a failure path
    // is a stall the operator did not ask for.
    std::string str() const;

private:
    sockaddr_storage storage_{};
    socklen_t size_ = 0;
};

// Resolve a target to every address worth trying, in the order the system
// prefers.
//
// Called ONCE per run, by the runner, and the result handed to every
// connection — which is why connect() takes a SocketAddress rather than an
// Endpoint. Resolving per connection would have 500 threads calling
// getaddrinfo at startup, and DNS failures would then land in the run's
// connect-error count looking exactly like the target refusing us.
//
// Throws IoError if the name does not resolve. That is a failure of the world,
// not of the invocation, so it is not a UsageError even though it arrives from
// a flag.
std::vector<SocketAddress> resolve(const Endpoint& endpoint);

}  // namespace dariyanaap
