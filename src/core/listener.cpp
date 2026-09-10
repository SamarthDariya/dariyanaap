#include "core/listener.hpp"

#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {
namespace {

// Bind one address, or throw saying which one and why.
Socket bind_one(const SocketAddress& address, int backlog) {
    const int fd = ::socket(address.family(), SOCK_STREAM, 0);
    if (fd < 0) {
        throw IoError("cannot create a socket for " + address.str() + ": " +
                      strerror(errno));
    }
    Socket socket = Socket::adopt(fd);  // owned before anything else can fail

    // Without SO_REUSEADDR, rebinding a port left in TIME_WAIT fails with
    // EADDRINUSE for up to a couple of minutes. E2 restarts the null target
    // between every step of the concurrency sweep, so this is the difference
    // between a sweep that runs and one that fails halfway with an error that
    // looks like a bug.
    const int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        throw IoError(string("setsockopt(SO_REUSEADDR): ") + strerror(errno));
    }

    if (::bind(fd, address.addr(), address.size()) < 0) {
        throw IoError("cannot bind " + address.str() + ": " + strerror(errno));
    }
    if (::listen(fd, backlog) < 0) {
        throw IoError("cannot listen on " + address.str() + ": " + strerror(errno));
    }
    return socket;
}

// The port the kernel actually gave us. For a fixed port this confirms it; for
// port 0 it is the only way to find out.
uint16_t bound_port(int fd) {
    sockaddr_storage storage = {};
    socklen_t size = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &size) < 0) {
        throw IoError(string("getsockname: ") + strerror(errno));
    }
    switch (storage.ss_family) {
        case AF_INET:
            return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
        case AF_INET6:
            return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
        default:
            throw IoError("bound an address family with no port");
    }
}

}  // namespace

Listener Listener::bind_first_that_works(const vector<SocketAddress>& addresses,
                                         int backlog) {
    string failures;
    for (const SocketAddress& address : addresses) {
        try {
            Socket socket = bind_one(address, backlog);
            const uint16_t port = bound_port(socket.fd());
            return Listener(std::move(socket), address.with_port(port), port);
        } catch (const IoError& failed) {
            if (!failures.empty()) failures += "; ";
            failures += failed.what();
        }
    }
    throw IoError(failures);
}

Listener::Listener(Socket socket, SocketAddress address, uint16_t port)
    : socket_(std::move(socket)), address_(std::move(address)), port_(port) {}

Listener Listener::bind(const Endpoint& endpoint, int backlog) {
    return bind_first_that_works(resolve(endpoint), backlog);
}

Listener Listener::bind_ephemeral(const string& host, int backlog) {
    // Endpoint refuses port 0, so resolve with a placeholder and replace it.
    // The placeholder is never used for anything: with_port overwrites it
    // before any syscall sees the address.
    vector<SocketAddress> addresses = resolve(Endpoint(host, 1));
    for (SocketAddress& address : addresses) {
        address = address.with_port(0);
    }
    return bind_first_that_works(addresses, backlog);
}

Socket Listener::accept() {
    for (;;) {
        const int accepted = ::accept(socket_.fd(), nullptr, nullptr);
        if (accepted >= 0) {
            // adopt, not the raw constructor: SO_NOSIGPIPE is per-socket and
            // an accepted descriptor does not inherit it.
            return Socket::adopt(accepted);
        }
        if (errno == EINTR) {
            continue;
        }
        throw IoError(string("accept on ") + address_.str() + ": " + strerror(errno));
    }
}

}  // namespace dariyanaap
