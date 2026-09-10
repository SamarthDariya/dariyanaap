#pragma once

#include <cstddef>
#include <span>

#include "core/address.hpp"
#include "core/units.hpp"

namespace dariyanaap {

// RAII owner of a socket file descriptor.
//
// Move-only, never copyable. Two owners of one fd means whichever is destroyed
// first closes it, and the other then reads and writes a descriptor the kernel
// has handed to something else — which surfaces as a plausible-looking network
// error rather than as the memory bug it is. A load generator whose errors
// might be its own is worth nothing, so copying is deleted rather than
// documented as unwise.
//
// There is no default constructor, and this is the one place the repo's "a
// value that exists is valid" rule (Rate, Endpoint, Summary) has to bend:
// moving *requires* an empty state, because the moved-from object still has to
// be destructible. So the empty state exists solely as the residue of a move,
// and valid() is how you tell. It cannot be reached any other way — the only
// public way to get a Socket is to be handed a live fd, or to move one.
class Socket {
public:
    // Takes ownership. Asserts on a negative fd: that is a caller who did not
    // check the syscall's return value, which is a bug, not a runtime error.
    explicit Socket(int fd);

    // Take ownership of a descriptor and apply the options every socket this
    // repo owns must have — currently just SIGPIPE suppression.
    //
    // Both connect() and Listener::accept() go through here, because
    // SO_NOSIGPIPE is per-socket and is NOT inherited by an accepted
    // descriptor. A server that set it only on its listener would die of
    // SIGPIPE the first time a client hung up mid-response, which is exactly
    // what a load generator does to a target at the end of every run.
    static Socket adopt(int fd);

    // Connect to one already-resolved address, giving up after `timeout`.
    //
    // Non-blocking connect plus poll, not a blocking connect, and the reason
    // is a number: macOS's default connect timeout is about 75 seconds and
    // cannot be shortened through any socket option. A rig whose connect can
    // stall for 75s cannot measure M4's hang_forever() — it would hang
    // alongside the target instead of recording that the target hung.
    //
    // Returns a socket back in BLOCKING mode. The non-blocking flag exists
    // only to bound this call; reads and writes get their bound from
    // SO_RCVTIMEO/SO_SNDTIMEO at chunk 2.3, which keeps the request path free
    // of readiness bookkeeping until M3 genuinely needs it.
    //
    // Throws TimedOut if the timeout expires, IoError for anything else.
    static Socket connect(const SocketAddress& address, Millis timeout);

    // Try each address in order until one connects.
    //
    // Not a convenience wrapper — a correctness requirement. On this machine
    // `localhost` resolves to [::1] FIRST and 127.0.0.1 second, so a rig that
    // only tried addresses[0] would fail against an IPv4-only target that is
    // running perfectly, and report it as the target refusing connections.
    //
    // The timeout is PER ADDRESS, so the worst case is timeout x addresses.
    // A shared budget was the obvious alternative and is worse: a black-holed
    // IPv6 address would consume the whole allowance and leave nothing for the
    // IPv4 address that would have worked.
    //
    // Throws TimedOut only if EVERY attempt timed out — that is the one case
    // where nothing definitive was learned. If any address refused, the target
    // is reachable and declining, which is a connect failure and a different
    // counter (decision 6). The message names every address tried and why each
    // failed, because "connect failed" against a name with three addresses
    // sends the operator to tcpdump.
    static Socket connect_any(const std::vector<SocketAddress>& addresses,
                              Millis timeout);
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    // Bound every read and write. Called once after connect, never per
    // request: setsockopt on the request path would be measured overhead.
    //
    // A read that expires throws TimedOut, which the runner records in the
    // histogram AT the timeout value rather than dropping. Dropping it is
    // coordinated omission arriving early — the request that took longest
    // would be the one that never appears in the distribution.
    void set_timeouts(Millis read_timeout, Millis write_timeout);

    // Read whatever has arrived, up to buffer.size() bytes.
    //
    // Returns 0 for a clean peer close, and that is NOT an error here: whether
    // a short response is a failure is a protocol question, so the decision
    // belongs to whoever knows the protocol (chunk 2.9), not to the socket.
    //
    // Throws TimedOut if the read timeout expires, IoError otherwise.
    std::size_t read_some(std::span<char> buffer);

    // Write all of it, looping over partial writes.
    //
    // If this throws, an unknown prefix has already reached the peer, so the
    // connection is unusable and the caller must drop it rather than send the
    // next request down a stream the peer is misparsing.
    void write_all(std::span<const char> data);

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }

    // Idempotent, so a runner that closes a connection early and then lets the
    // object go out of scope does not double-close a descriptor the kernel may
    // have already reissued.
    void close();

    // Give up ownership without closing, for handing an fd to something that
    // will own it instead. Returns -1 if already empty.
    int release();

private:
    int fd_;
};

}  // namespace dariyanaap
