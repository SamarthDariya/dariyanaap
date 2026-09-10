#pragma once

#include <cstdint>
#include <string>

#include "core/address.hpp"
#include "core/endpoint.hpp"
#include "core/socket.hpp"

namespace dariyanaap {

// A listening socket. Used by dariyanaap-null, the calibration target, and by
// fault at M4 — which is why it is core and not load.
//
// Holds a Socket rather than a raw fd, so RAII and move-only come for free and
// there is one place in the repo that closes a descriptor. The Socket is
// private: read_some and write_all are meaningless on a listener, and
// inheriting them would invite calling one.
class Listener {
public:
    // macOS clamps the listen backlog to kern.ipc.somaxconn silently, which is
    // 128 by default. Asking for more is not an error and does not warn — it
    // just gets you 128. This matters for E2: a ramp to 1000 connections
    // offers far more pending connects than the queue holds, and the ones that
    // do not fit are refused or dropped. That is a RIG artifact, not a target
    // failure, and mistaking it for one would corrupt the number E2 exists to
    // establish. Raising it needs `sudo sysctl -w kern.ipc.somaxconn=...`.
    static constexpr int kDefaultBacklog = 128;

    // Bind and listen on a fixed port.
    //
    // Tries each resolved address in order, as connect_any does, so a host
    // with no usable IPv6 route falls through to IPv4 instead of failing. Give
    // a literal address rather than a name if the family matters — "localhost"
    // resolves IPv6-first on macOS, so it binds IPv6-only.
    static Listener bind(const Endpoint& endpoint, int backlog = kDefaultBacklog);

    // Bind to a kernel-chosen port. port() then reports what it got.
    //
    // Takes a host string rather than an Endpoint because Endpoint refuses
    // port 0 — see SocketAddress::with_port, which is how the 0 gets in.
    static Listener bind_ephemeral(const std::string& host,
                                   int backlog = kDefaultBacklog);

    // Block until a client connects. The returned socket is adopted, so it has
    // SIGPIPE suppressed — accept() does not inherit SO_NOSIGPIPE.
    Socket accept();

    // The port actually bound, which is the only way to learn an ephemeral one.
    std::uint16_t port() const { return port_; }

    // What to tell an operator to connect to.
    const SocketAddress& address() const { return address_; }

    int fd() const { return socket_.fd(); }

private:
    Listener(Socket socket, SocketAddress address, std::uint16_t port);

    // A member rather than a free helper, because it is the only thing that
    // may call the private constructor — which is what keeps every Listener
    // in existence bound and listening.
    static Listener bind_first_that_works(const std::vector<SocketAddress>& addresses,
                                          int backlog);

    Socket socket_;
    SocketAddress address_;
    std::uint16_t port_;
};

}  // namespace dariyanaap
