#include "core/address.hpp"

#include <netdb.h>

#include <cstring>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

SocketAddress::SocketAddress(const sockaddr* addr, socklen_t size) : size_(size) {
    memcpy(&storage_, addr, size);
}

const sockaddr* SocketAddress::addr() const {
    return reinterpret_cast<const sockaddr*>(&storage_);
}

int SocketAddress::family() const {
    return storage_.ss_family;
}

string SocketAddress::str() const {
    char host[NI_MAXHOST] = {};
    char port[NI_MAXSERV] = {};
    // NUMERICHOST and NUMERICSERV: no reverse lookup, no /etc/services.
    if (getnameinfo(addr(), size_, host, sizeof(host), port, sizeof(port),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "<unprintable address>";
    }
    return family() == AF_INET6 ? "[" + string(host) + "]:" + port
                                : string(host) + ":" + port;
}

vector<SocketAddress> resolve(const Endpoint& endpoint) {
    addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;      // v4 or v6, whichever the system prefers
    hints.ai_socktype = SOCK_STREAM;
    // NUMERICSERV because Endpoint has already parsed the port into a number,
    // so there is nothing for /etc/services to add. ADDRCONFIG so a machine
    // with no IPv6 route is not handed IPv6 addresses that cannot connect.
    hints.ai_flags = AI_NUMERICSERV | AI_ADDRCONFIG;

    addrinfo* results = nullptr;
    const string port = to_string(endpoint.port());
    const int rc = getaddrinfo(endpoint.host().c_str(), port.c_str(), &hints, &results);
    if (rc != 0) {
        throw IoError("cannot resolve " + endpoint.str() + ": " + gai_strerror(rc));
    }

    vector<SocketAddress> out;
    for (const addrinfo* it = results; it != nullptr; it = it->ai_next) {
        out.emplace_back(it->ai_addr, it->ai_addrlen);
    }
    freeaddrinfo(results);

    if (out.empty()) {
        // getaddrinfo returning success with no addresses should be
        // impossible; saying so beats handing back an empty list that the
        // runner would read as "no connections to make".
        throw IoError("resolved " + endpoint.str() + " to no addresses");
    }
    return out;
}

}  // namespace dariyanaap
