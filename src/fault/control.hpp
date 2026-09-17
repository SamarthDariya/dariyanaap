#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace dariyanaap::fault {

// A socket that lets a run change faults while the target is serving.
//
// DESIGN.md decision 10, and the reason M4 is a day rather than an hour. None
// of these are expressible as startup configuration:
//
//   unit 2  makes a healthy backend hang_forever() in the middle of a run
//   unit 8  partitions a cluster, writes to both sides, then heals it
//   unit 9  makes a dependency slow for ten seconds and watches a breaker
//           trip and recover
//
// All of them are the interesting part of their unit. Env-only would have been
// the two-day version of this repo; this is what makes it the three-day one.
//
// One line per command, one line of reply. No framing, no length prefix, no
// concurrent clients — a control plane is not the subject of any experiment
// here, and rule 5 says cap the scope.
class ControlServer {
public:
    // Bind, listen, and serve on a thread until stopped. Throws IoError if the
    // port is unavailable.
    ControlServer(const std::string& host, std::uint16_t port);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // The port actually bound, for when 0 was asked for.
    std::uint16_t port() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace dariyanaap::fault
