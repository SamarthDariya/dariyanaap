#pragma once

#include <cstddef>
#include <vector>

#include "load/protocol.hpp"

namespace dariyanaap {

// Send N bytes, expect N bytes back.
//
// The protocol for calibration (E2) and for the units that only need traffic
// rather than semantics — the L4 proxy in unit 2, the log in unit 6, the
// quorum store in unit 8. It exists to make the TARGET's work as close to zero
// as a socket allows, so the number E2 produces is the rig's ceiling and not
// the server's.
//
// It verifies length, not content, and that is deliberate: a memcmp per
// request would be the rig doing work on the measured path, and it would land
// in the reported latency as though the target had caused it. Whether the echo
// is byte-correct is a question for the test suite, which checks it once,
// rather than for a run that asks it a million times.
class RawEcho : public Protocol {
public:
    explicit RawEcho(std::size_t size);

    std::span<const char> request() const override;
    ResponseState consume(std::span<const char> response) const override;
    bool succeeded(std::span<const char> response) const override;

    std::size_t size() const { return payload_.size(); }

private:
    std::vector<char> payload_;
};

}  // namespace dariyanaap
