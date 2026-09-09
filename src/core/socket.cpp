#include "core/socket.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <cstring>
#include <utility>

#include "core/clock.hpp"
#include "core/errors.hpp"

using namespace std;

namespace dariyanaap {

Socket::Socket(int fd) : fd_(fd) {
    assert(fd >= 0 && "Socket built from a failed syscall: check the return value first");
}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) {
    other.fd_ = -1;
}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        // Close what we already hold first. Forgetting this is the classic
        // move-assignment leak: the old descriptor stays open for the life of
        // the process, and a run that reconnects on error would exhaust the
        // fd table partway through and report the result as target failures.
        close();
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        // The return value is deliberately ignored. close() can fail with
        // EINTR on macOS, but the descriptor is closed either way, so
        // retrying would close whatever the kernel has since reissued.
        ::close(fd_);
        fd_ = -1;
    }
}

int Socket::release() {
    return exchange(fd_, -1);
}

namespace {

string describe(const char* what, const SocketAddress& address, int code) {
    return string(what) + " " + address.str() + ": " + strerror(code);
}

// Wait until the connect resolves one way or the other, or the deadline
// passes. Retries on EINTR against a deadline rather than restarting the
// timeout, so a stray signal cannot turn a 100ms bound into an unbounded wait
// — and cannot report a timeout that never happened.
void await_writable(int fd, const SocketAddress& address, Millis timeout) {
    const MonotonicClock::Instant deadline = MonotonicClock::now() + timeout;
    for (;;) {
        const Nanos left = deadline - MonotonicClock::now();
        if (left <= Nanos(0)) {
            throw TimedOut("connect to " + address.str() + " timed out after " +
                           to_string(timeout.count()) + "ms");
        }
        pollfd waiting = {fd, POLLOUT, 0};
        const int ready = ::poll(&waiting, 1, static_cast<int>(
                                     chrono::duration_cast<Millis>(left).count()));
        if (ready > 0) {
            return;
        }
        if (ready == 0) {
            throw TimedOut("connect to " + address.str() + " timed out after " +
                           to_string(timeout.count()) + "ms");
        }
        if (errno != EINTR) {
            throw IoError(describe("poll while connecting to", address, errno));
        }
    }
}

void set_nonblocking(int fd, bool on) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        throw IoError(string("fcntl F_GETFL: ") + strerror(errno));
    }
    const int wanted = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(fd, F_SETFL, wanted) < 0) {
        throw IoError(string("fcntl F_SETFL: ") + strerror(errno));
    }
}

}  // namespace

Socket Socket::connect(const SocketAddress& address, Millis timeout) {
    const int fd = ::socket(address.family(), SOCK_STREAM, 0);
    if (fd < 0) {
        throw IoError(describe("cannot create a socket for", address, errno));
    }

    // Owned before anything else can fail. Every throw below now closes the
    // descriptor on the way out, which is the entire reason chunk 2.1 exists —
    // a run that reconnects on error would otherwise leak an fd per failure
    // and start reporting fd exhaustion as target failures.
    Socket socket(fd);

    set_nonblocking(fd, true);

    if (::connect(fd, address.addr(), address.size()) != 0) {
        if (errno != EINPROGRESS) {
            throw IoError(describe("connect to", address, errno));
        }
        await_writable(fd, address, timeout);

        // poll said writable, which does NOT mean connected: a refused
        // connection also makes the descriptor writable. SO_ERROR is the only
        // way to tell them apart, and skipping it is the classic non-blocking
        // connect bug — every request then fails on a socket that was never
        // connected, and the run reports read errors against a target that
        // refused at the door.
        int error = 0;
        socklen_t size = sizeof(error);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) {
            throw IoError(describe("getsockopt(SO_ERROR) for", address, errno));
        }
        if (error != 0) {
            throw IoError(describe("connect to", address, error));
        }
    }

    set_nonblocking(fd, false);
    return socket;
}

}  // namespace dariyanaap
