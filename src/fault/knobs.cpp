#include "fault/knobs.hpp"

#include <atomic>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <utility>

#include "core/errors.hpp"

using namespace std;

namespace dariyanaap::fault {
namespace {

// The one gate. Every check reads this first, so all-faults-off costs a single
// relaxed load and nothing else.
atomic<bool> g_any{false};

atomic<int64_t> g_latency_ns{0};
atomic<int64_t> g_jitter_ns{0};
atomic<int64_t> g_drop_ppm{0};  // parts per million, so one atomic not two
atomic<bool> g_hanging{false};

mutex g_partition_lock;
string g_identity;                        // guarded by g_partition_lock
set<pair<string, string>> g_partitions;   // guarded by g_partition_lock
atomic<bool> g_any_partition{false};

// Per-thread, so drawing jitter or a drop decision never contends. A shared
// generator would put a lock on the path this library promises is nearly free.
thread_local mt19937_64 t_random{random_device{}()};

void refresh_gate() {
    g_any.store(g_latency_ns.load(memory_order_relaxed) != 0 ||
                    g_drop_ppm.load(memory_order_relaxed) != 0 ||
                    g_hanging.load(memory_order_relaxed) ||
                    g_any_partition.load(memory_order_relaxed),
                memory_order_relaxed);
}

// Partitions are stored with the names in a fixed order, so partition(a, b)
// and partition(b, a) are the same cut. A directional partition is a different
// experiment and this library does not offer one.
pair<string, string> ordered(string_view a, string_view b) {
    string first(a);
    string second(b);
    if (second < first) {
        swap(first, second);
    }
    return {std::move(first), std::move(second)};
}

}  // namespace

bool any_enabled() {
    return g_any.load(memory_order_relaxed);
}

void before_response() {
    if (!g_any.load(memory_order_relaxed)) {
        return;
    }

    const int64_t latency = g_latency_ns.load(memory_order_relaxed);
    if (latency > 0) {
        const int64_t jitter = g_jitter_ns.load(memory_order_relaxed);
        int64_t wait = latency;
        if (jitter > 0) {
            uniform_int_distribution<int64_t> spread(-jitter, jitter);
            wait += spread(t_random);
        }
        if (wait > 0) {
            this_thread::sleep_for(Nanos(wait));
        }
    }

    // Checked in a loop rather than slept on once, so heal takes effect
    // immediately. A hang that outlived its own knob would make unit 2's
    // "restart the backend mid-run" unstageable.
    while (g_hanging.load(memory_order_relaxed)) {
        this_thread::sleep_for(Millis(1));
    }
}

bool should_drop() {
    if (!g_any.load(memory_order_relaxed)) {
        return false;
    }
    const int64_t ppm = g_drop_ppm.load(memory_order_relaxed);
    if (ppm <= 0) {
        return false;
    }
    uniform_int_distribution<int64_t> draw(0, 999'999);
    return draw(t_random) < ppm;
}

bool blocked(string_view peer) {
    // Two gates, because taking the mutex would defeat the first one. The
    // common case in unit 8 is a healthy cluster, where this is two relaxed
    // loads and no lock at all.
    if (!g_any.load(memory_order_relaxed) ||
        !g_any_partition.load(memory_order_relaxed)) {
        return false;
    }
    const lock_guard<mutex> held(g_partition_lock);
    if (g_identity.empty()) {
        return false;  // nobody told us who we are, so nothing is about us
    }
    return g_partitions.count(ordered(g_identity, peer)) > 0;
}

void set_latency(Millis mean, Millis jitter) {
    if (mean.count() < 0 || jitter.count() < 0) {
        throw UsageError("injected latency and jitter must not be negative");
    }
    if (jitter > mean) {
        // Otherwise the draw would want a negative delay, and clamping it
        // silently would mean the configured mean was not the mean.
        throw UsageError("injected jitter must not exceed the mean");
    }
    g_latency_ns.store(Nanos(mean).count(), memory_order_relaxed);
    g_jitter_ns.store(Nanos(jitter).count(), memory_order_relaxed);
    refresh_gate();
}

void set_drop_probability(double probability) {
    if (!(probability >= 0.0 && probability <= 1.0)) {
        throw UsageError("drop probability must be between 0 and 1");
    }
    g_drop_ppm.store(static_cast<int64_t>(probability * 1'000'000.0 + 0.5),
                     memory_order_relaxed);
    refresh_gate();
}

void set_hang_forever(bool hanging) {
    g_hanging.store(hanging, memory_order_relaxed);
    refresh_gate();
}

void set_identity(string name) {
    const lock_guard<mutex> held(g_partition_lock);
    g_identity = std::move(name);
}

void partition(string_view a, string_view b) {
    if (a.empty() || b.empty()) {
        throw UsageError("a partition needs two node names");
    }
    if (a == b) {
        throw UsageError("cannot partition a node from itself");
    }
    {
        const lock_guard<mutex> held(g_partition_lock);
        g_partitions.insert(ordered(a, b));
        g_any_partition.store(true, memory_order_relaxed);
    }
    refresh_gate();
}

void heal(string_view a, string_view b) {
    {
        const lock_guard<mutex> held(g_partition_lock);
        g_partitions.erase(ordered(a, b));
        g_any_partition.store(!g_partitions.empty(), memory_order_relaxed);
    }
    refresh_gate();
}

void clear() {
    // Identity is not touched. See the header: a node's name is configuration,
    // and a reset that made a cluster forget its members would be worse than
    // the fault being staged.
    g_latency_ns.store(0, memory_order_relaxed);
    g_jitter_ns.store(0, memory_order_relaxed);
    g_drop_ppm.store(0, memory_order_relaxed);
    g_hanging.store(false, memory_order_relaxed);
    {
        const lock_guard<mutex> held(g_partition_lock);
        g_partitions.clear();
        g_any_partition.store(false, memory_order_relaxed);
    }
    refresh_gate();
}

}  // namespace dariyanaap::fault
