#include "fault/control.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "core/errors.hpp"
#include "core/listener.hpp"
#include "fault/knobs.hpp"

using namespace std;

namespace dariyanaap::fault {

struct ControlServer::State {
    Listener listener;
    atomic<bool> stopping{false};
    thread acceptor;

    explicit State(Listener bound) : listener(std::move(bound)) {}
};

namespace {

// One connection: read lines, apply each, reply. A malformed command is
// answered with its error rather than closing the connection — an operator
// mid-experiment should be able to retype it, not reconnect.
void serve(Socket client) {
    string pending;
    vector<char> buffer(4096);
    for (;;) {
        size_t got = 0;
        try {
            got = client.read_some({buffer.data(), buffer.size()});
        } catch (const IoError&) {
            return;
        }
        if (got == 0) {
            return;
        }
        pending.append(buffer.data(), got);

        size_t newline = pending.find('\n');
        while (newline != string::npos) {
            string line = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            string reply;
            try {
                reply = apply_command(line);
            } catch (const Error& e) {
                reply = string("error: ") + e.what();
            }
            reply += '\n';
            try {
                client.write_all({reply.data(), reply.size()});
            } catch (const IoError&) {
                return;
            }
            newline = pending.find('\n');
        }
    }
}

}  // namespace

ControlServer::ControlServer(const string& host, uint16_t port)
    : state_(make_unique<State>(port == 0 ? Listener::bind_ephemeral(host)
                                          : Listener::bind(Endpoint(host, port)))) {
    state_->acceptor = thread([state = state_.get()] {
        for (;;) {
            Socket client = [state]() -> Socket {
                return state->listener.accept();
            }();
            if (state->stopping.load(memory_order_relaxed)) {
                return;
            }
            // Timeouts on the control connection too: an operator who opens it
            // and walks away must not hold the thread for the whole run.
            client.set_timeouts(Millis(600'000), Millis(10'000));
            serve(std::move(client));
        }
    });
}

ControlServer::~ControlServer() {
    state_->stopping.store(true, memory_order_relaxed);
    try {
        // Unblock the accept. The acceptor sees `stopping` and returns.
        const Socket poke = Socket::connect_any(
            resolve(Endpoint("127.0.0.1", state_->listener.port())), Millis(500));
    } catch (const IoError&) {
        // Nothing to unblock, or already gone.
    }
    if (state_->acceptor.joinable()) {
        state_->acceptor.join();
    }
}

uint16_t ControlServer::port() const {
    return state_->listener.port();
}

}  // namespace dariyanaap::fault
