#include "fault/knobs.hpp"

#include <atomic>
#include <optional>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <cstdlib>
#include <sstream>
#include <utility>
#include <vector>

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

namespace {

// Read one variable, or nothing. A variable that is set but unreadable is an
// error rather than a default: see load_from_env's comment.
optional<string> from_env(const char* name) {
    const char* value = getenv(name);
    if (value == nullptr || *value == '\0') {
        return nullopt;
    }
    return string(value);
}

double as_probability(const string& text, const char* what) {
    istringstream in(text);
    double value = 0.0;
    in >> value;
    if (in.fail() || !in.eof()) {
        throw UsageError(string(what) + " must be a number, got \"" + text + "\"");
    }
    return value;
}

int64_t as_millis(const string& text, const char* what) {
    istringstream in(text);
    long long value = 0;
    in >> value;
    if (in.fail() || !in.eof()) {
        throw UsageError(string(what) + " must be a whole number of ms, got \"" + text + "\"");
    }
    return value;
}

vector<string> split_on(string_view text, char separator) {
    vector<string> parts;
    string current;
    for (const char c : text) {
        if (c == separator) {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

}  // namespace

void load_from_env() {
    if (const optional<string> name = from_env("DARIYANAAP_FAULT_IDENTITY")) {
        set_identity(*name);
    }

    const optional<string> latency = from_env("DARIYANAAP_FAULT_LATENCY_MS");
    const optional<string> jitter = from_env("DARIYANAAP_FAULT_JITTER_MS");
    if (latency || jitter) {
        set_latency(Millis(latency ? as_millis(*latency, "DARIYANAAP_FAULT_LATENCY_MS") : 0),
                    Millis(jitter ? as_millis(*jitter, "DARIYANAAP_FAULT_JITTER_MS") : 0));
    }
    if (const optional<string> drop = from_env("DARIYANAAP_FAULT_DROP")) {
        set_drop_probability(as_probability(*drop, "DARIYANAAP_FAULT_DROP"));
    }
    if (const optional<string> hang = from_env("DARIYANAAP_FAULT_HANG")) {
        set_hang_forever(*hang == "1" || *hang == "true");
    }
    if (const optional<string> cuts = from_env("DARIYANAAP_FAULT_PARTITION")) {
        for (const string& cut : split_on(*cuts, ',')) {
            const vector<string> pair_of = split_on(cut, ':');
            if (pair_of.size() != 2) {
                throw UsageError("DARIYANAAP_FAULT_PARTITION wants \"a:b\", got \"" + cut + "\"");
            }
            partition(pair_of[0], pair_of[1]);
        }
    }
}

string apply_command(string_view line) {
    const vector<string> words = split_on(line, ' ');
    if (words.empty()) {
        throw UsageError("empty command");
    }
    const string& verb = words[0];

    auto expect = [&words, &verb](size_t count) {
        if (words.size() != count + 1) {
            throw UsageError(verb + " takes " + to_string(count) + " argument(s)");
        }
    };

    if (verb == "latency") {
        expect(2);
        set_latency(Millis(as_millis(words[1], "latency")),
                    Millis(as_millis(words[2], "jitter")));
        return "ok";
    }
    if (verb == "drop") {
        expect(1);
        set_drop_probability(as_probability(words[1], "drop"));
        return "ok";
    }
    if (verb == "hang") {
        expect(1);
        set_hang_forever(words[1] == "1" || words[1] == "true");
        return "ok";
    }
    if (verb == "identity") {
        expect(1);
        set_identity(words[1]);
        return "ok";
    }
    if (verb == "partition") {
        expect(2);
        partition(words[1], words[2]);
        return "ok";
    }
    if (verb == "heal") {
        expect(2);
        heal(words[1], words[2]);
        return "ok";
    }
    if (verb == "clear") {
        expect(0);
        clear();
        return "ok";
    }
    if (verb == "status") {
        expect(0);
        return any_enabled() ? "faults active" : "no faults";
    }
    // Listing the verbs, for the reason the CLI lists its flags: the usual
    // cause is a near miss, and a command that silently did nothing would
    // leave a run looking like the experiment without being it.
    throw UsageError("unknown command \"" + verb +
                     "\"; try latency, drop, hang, identity, partition, heal, clear, status");
}

}  // namespace dariyanaap::fault
