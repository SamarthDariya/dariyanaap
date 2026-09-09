#include "core/socket.hpp"

#include <unistd.h>

#include <cassert>
#include <utility>

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

}  // namespace dariyanaap
