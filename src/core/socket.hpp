#pragma once

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
    ~Socket();

    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

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
