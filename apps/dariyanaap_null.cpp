// dariyanaap-null — a target that replies as fast as a socket allows.
//
// Exists for E2. Decision 7 says the rig publishes its own ceiling, and you
// cannot measure that against a target that does real work: the number would
// be the server's limit, not the rig's. So this does the least a TCP service
// can do — read N bytes, write the same N bytes back, forever.
//
// It speaks RawEcho only, never HTTP. Parsing a request line and formatting a
// status line is work, and E2 needs the target's share of each request to be
// as close to zero as possible.
//
//     dariyanaap-null                       127.0.0.1, kernel-chosen port
//     dariyanaap-null --port 9000           a fixed port
//     dariyanaap-null --payload 256
//     dariyanaap-null --stall-every 1000 --stall-for 200
//
// The stall knobs exist for E3. A target that freezes periodically is what
// separates closed-loop from open-loop measurement, and E3 needs one before
// M4's fault library exists. M4 generalises this into something other
// people's services can link; this is the version that only has to stall
// itself.
//
// The bound address is printed on the first line of stdout and flushed, so a
// sweep script can read the port back when it asked for an ephemeral one.

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "core/errors.hpp"
#include "core/flags.hpp"
#include "core/listener.hpp"
#include "core/socket.hpp"

using namespace dariyanaap;
using namespace std;

namespace {

// Echo `payload` bytes at a time until the client goes away.
//
// A connection ending is not an error here. A load generator hangs up on every
// connection at the end of every run, so treating that as a failure would make
// the target noisy exactly when it is working.
// Set by the stall thread. Read once per request by every connection, which
// is one relaxed load against a syscall pair — the same argument DESIGN.md
// decision 9 makes for the fault library at M4.
atomic<bool> g_stalled{false};

// Freeze every connection for `duration`, every `period`. One thread flips a
// flag rather than each connection sleeping on its own schedule, so all
// connections stall together — which is what a real service does when it
// blocks on a lock, a GC pause, or a slow dependency.
void stall_periodically(Millis period, Millis duration) {
    for (;;) {
        this_thread::sleep_for(period);
        g_stalled.store(true, memory_order_relaxed);
        this_thread::sleep_for(duration);
        g_stalled.store(false, memory_order_relaxed);
    }
}

void serve(Socket client, size_t payload) {
    vector<char> buffer(payload > 64 * 1024 ? payload : 64 * 1024);
    try {
        for (;;) {
            size_t have = 0;
            while (have < payload) {
                const size_t got = client.read_some(
                    {buffer.data() + have, buffer.size() - have});
                if (got == 0) {
                    return;  // clean hang-up
                }
                have += got;
            }
            while (g_stalled.load(memory_order_relaxed)) {
                // Hold the reply. Not a sleep of fixed length: the connection
                // resumes the moment the stall ends, so a 200ms stall delays a
                // request by however much of it remained when the request
                // arrived — which is what a real freeze does.
                this_thread::sleep_for(Millis(1));
            }
            client.write_all({buffer.data(), payload});
        }
    } catch (const IoError&) {
        // The client vanished mid-exchange. Its problem, not ours.
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (Flags::wants_help(argc, argv)) {
            fputs("usage: dariyanaap-null [options]\n"
                  "  --host H            bind address (default 127.0.0.1)\n"
                  "  --port P            0 for a kernel-chosen port (default 0)\n"
                  "  --payload N         bytes echoed each way (default 64)\n"
                  "  --backlog N         listen backlog; macOS clamps to somaxconn\n"
                  "  --stall-every MS    freeze every connection periodically\n"
                  "  --stall-for MS      for this long (both or neither)\n",
                  stdout);
            return 0;
        }
        const Flags flags = Flags::parse(
            argc, argv,
            {"host", "port", "payload", "backlog", "stall-every", "stall-for"});
        const string host = flags.text("host", "127.0.0.1");
        const uint64_t port = flags.number("port", 0);
        const uint64_t payload = flags.number("payload", 64);
        const uint64_t backlog =
            flags.number("backlog", static_cast<uint64_t>(Listener::kDefaultBacklog));

        if (port > 65535) {
            throw UsageError("--port must be at most 65535");
        }
        if (payload == 0) {
            throw UsageError("--payload must be at least 1 byte");
        }

        // Port 0 goes through bind_ephemeral because Endpoint refuses it: as a
        // destination it is meaningless, and only a bind address can mean
        // "kernel, choose one" (chunk 2.4a).
        Listener listener =
            port == 0
                ? Listener::bind_ephemeral(host, static_cast<int>(backlog))
                : Listener::bind(Endpoint(host, static_cast<uint16_t>(port)),
                                 static_cast<int>(backlog));

        // First line, flushed: a sweep script reads the port from here.
        const uint64_t stall_every = flags.number("stall-every", 0);
        const uint64_t stall_for = flags.number("stall-for", 0);
        if ((stall_every == 0) != (stall_for == 0)) {
            throw UsageError("--stall-every and --stall-for must be given together");
        }
        thread staller;
        if (stall_every > 0) {
            staller = thread(stall_periodically, Millis(static_cast<int64_t>(stall_every)),
                             Millis(static_cast<int64_t>(stall_for)));
            staller.detach();
        }

        printf("listening %s payload %llu backlog %llu stall %llu/%llu ms\n",
               listener.address().str().c_str(),
               static_cast<unsigned long long>(payload),
               static_cast<unsigned long long>(backlog),
               static_cast<unsigned long long>(stall_for),
               static_cast<unsigned long long>(stall_every));
        fflush(stdout);

        // Thread per connection, matching the rig's own naive model at M2. It
        // is the wrong design for a real server and the right one here: the
        // point is to add no latency of our own, and a thread blocked in recv
        // adds none.
        vector<thread> connections;
        for (;;) {
            Socket client = listener.accept();
            // Accepted sockets inherit no timeouts. Without one, a client that
            // connects and never speaks holds a thread forever, and a sweep
            // that runs six steps leaks six threads' worth of them.
            client.set_timeouts(Millis(30'000), Millis(30'000));
            connections.emplace_back([client = std::move(client), payload]() mutable {
                serve(std::move(client), static_cast<size_t>(payload));
            });
            // Reap finished threads occasionally so a long sweep does not
            // accumulate joinable handles for every connection ever made.
            if (connections.size() >= 4096) {
                for (thread& finished : connections) {
                    finished.detach();
                }
                connections.clear();
            }
        }
    } catch (const UsageError& e) {
        fprintf(stderr, "dariyanaap-null: %s\n", e.what());
        return 2;
    } catch (const Error& e) {
        fprintf(stderr, "dariyanaap-null: %s\n", e.what());
        return 1;
    }
}
