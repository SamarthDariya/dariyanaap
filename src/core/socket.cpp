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

// Without this, writing to a socket the peer has closed raises SIGPIPE, whose
// default disposition terminates the process. A load generator that dies the
// first time a target closes a connection is not a load generator — and unit 2
// deliberately kills backends mid-run, so this is not a rare path.
//
// SO_NOSIGPIPE is the BSD/macOS spelling. Linux has no such option and uses
// MSG_NOSIGNAL per-send instead, so a Linux port changes this function and the
// two send() calls, and nothing else.
void disable_sigpipe(int fd) {
    const int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) < 0) {
        throw IoError(string("setsockopt(SO_NOSIGPIPE): ") + strerror(errno));
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

    disable_sigpipe(fd);
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

Socket Socket::connect_any(const vector<SocketAddress>& addresses, Millis timeout) {
    // resolve() throws rather than returning an empty list, so an empty vector
    // here is a caller who built one by hand — a bug, not a runtime failure.
    assert(!addresses.empty() && "connect_any needs at least one address");

    string failures;
    bool every_attempt_timed_out = true;
    for (const SocketAddress& address : addresses) {
        try {
            return connect(address, timeout);
        } catch (const TimedOut& timed_out) {
            // Caught before IoError, which it derives from. Reversing these
            // two clauses would make the TimedOut branch dead code.
            if (!failures.empty()) failures += "; ";
            failures += timed_out.what();
        } catch (const IoError& failed) {
            every_attempt_timed_out = false;
            if (!failures.empty()) failures += "; ";
            failures += failed.what();
        }
    }

    if (every_attempt_timed_out) {
        throw TimedOut(failures);
    }
    throw IoError(failures);
}

void Socket::set_timeouts(Millis read_timeout, Millis write_timeout) {
    assert(valid() && "set_timeouts on a moved-from socket");

    auto apply = [this](int option, Millis value, const char* name) {
        timeval tv = {};
        tv.tv_sec = value.count() / 1000;
        tv.tv_usec = static_cast<int>((value.count() % 1000) * 1000);
        if (::setsockopt(fd_, SOL_SOCKET, option, &tv, sizeof(tv)) < 0) {
            throw IoError(string("setsockopt(") + name + "): " + strerror(errno));
        }
    };
    apply(SO_RCVTIMEO, read_timeout, "SO_RCVTIMEO");
    apply(SO_SNDTIMEO, write_timeout, "SO_SNDTIMEO");
}

size_t Socket::read_some(span<char> buffer) {
    assert(valid() && "read_some on a moved-from socket");
    assert(!buffer.empty() && "read_some into an empty buffer reads nothing forever");

    for (;;) {
        const ssize_t got = ::recv(fd_, buffer.data(), buffer.size(), 0);
        if (got >= 0) {
            return static_cast<size_t>(got);
        }
        if (errno == EINTR) {
            continue;  // a signal, not a network event
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // The socket is blocking (connect restored that), so EAGAIN here
            // can only mean SO_RCVTIMEO expired. On a non-blocking socket the
            // same errno would mean "nothing yet", which is why the blocking
            // restore at the end of connect() is load-bearing rather than
            // tidiness.
            throw TimedOut("read timed out");
        }
        throw IoError(string("read: ") + strerror(errno));
    }
}

void Socket::write_all(span<const char> data) {
    assert(valid() && "write_all on a moved-from socket");

    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote = ::send(fd_, data.data() + sent, data.size() - sent, 0);
        if (wrote > 0) {
            sent += static_cast<size_t>(wrote);
            continue;
        }
        if (wrote == 0) {
            // send() returning 0 for a non-empty buffer should not happen on a
            // stream socket. Looping on it would spin forever, so say so.
            throw IoError("write made no progress");
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            throw TimedOut("write timed out after " + to_string(sent) + " of " +
                           to_string(data.size()) + " bytes");
        }
        throw IoError(string("write: ") + strerror(errno));
    }
}

}  // namespace dariyanaap
