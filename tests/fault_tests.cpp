#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/errors.hpp"
#include "core/socket.hpp"
#include "fault/control.hpp"
#include "fault/knobs.hpp"

using namespace dariyanaap;

namespace {

// The knobs are process-global, as they must be — a target's request path
// cannot be handed a context object it does not have. So every case clears
// them, and the suite must not be run with doctest's parallel runner.
struct Clean {
    Clean() { reset(); }
    ~Clean() { reset(); }

    // Identity as well as the faults. clear() leaves identity alone on
    // purpose — it is configuration, not a fault — and the first version of
    // this fixture forgot that, so a case that set identity to "node-b" made a
    // later case see a partition as being about itself. Test isolation is the
    // fixture's job, not clear()'s.
    static void reset() {
        fault::clear();
        fault::set_identity("");
    }
};

}  // namespace

TEST_CASE("with nothing enabled, every check is a no-op") {
    const Clean clean;
    CHECK_FALSE(fault::any_enabled());
    CHECK_FALSE(fault::should_drop());
    CHECK_FALSE(fault::blocked("node-b"));

    const Stopwatch watch;
    for (int i = 0; i < 100'000; ++i) {
        fault::before_response();
    }
    // 100,000 calls through the disabled path. One relaxed load each, so this
    // is microseconds; a millisecond would mean the gate is not working.
    CHECK(watch.elapsed() < Millis(50));
}

TEST_CASE("injected latency delays a response by about what was asked") {
    const Clean clean;
    fault::set_latency(Millis(40), Millis(0));
    CHECK(fault::any_enabled());

    const Stopwatch watch;
    fault::before_response();
    const Nanos elapsed = watch.elapsed();
    CHECK(elapsed >= Millis(38));
    CHECK(elapsed < Millis(120));   // generous: sleep_for only promises a floor
}

TEST_CASE("jitter spreads the delay instead of releasing everyone together") {
    // A fixed delay makes every affected request finish in lockstep, which
    // produces a thundering herd the experiment did not ask for and a
    // histogram with one spike instead of a distribution.
    const Clean clean;
    fault::set_latency(Millis(20), Millis(10));

    std::vector<std::int64_t> samples;
    for (int i = 0; i < 12; ++i) {
        const Stopwatch watch;
        fault::before_response();
        samples.push_back(std::chrono::duration_cast<Millis>(watch.elapsed()).count());
    }
    std::int64_t lowest = samples[0];
    std::int64_t highest = samples[0];
    for (const std::int64_t s : samples) {
        lowest = std::min(lowest, s);
        highest = std::max(highest, s);
    }
    CHECK(lowest >= 9);           // mean 20 minus jitter 10, minus rounding
    CHECK(highest > lowest);      // not all identical
}

TEST_CASE("latency and jitter are validated, since a bad pair has no meaning") {
    const Clean clean;
    CHECK_THROWS_AS(fault::set_latency(Millis(-1), Millis(0)), UsageError);
    CHECK_THROWS_AS(fault::set_latency(Millis(10), Millis(-1)), UsageError);
    // Jitter past the mean would want a negative delay, and clamping it
    // silently would mean the configured mean was not the mean.
    CHECK_THROWS_AS(fault::set_latency(Millis(10), Millis(11)), UsageError);
    CHECK_NOTHROW(fault::set_latency(Millis(10), Millis(10)));
}

TEST_CASE("drop probability drops about that fraction") {
    const Clean clean;
    fault::set_drop_probability(0.25);

    int dropped = 0;
    for (int i = 0; i < 20'000; ++i) {
        if (fault::should_drop()) ++dropped;
    }
    // 25% of 20,000 is 5,000; three sigma is about 185.
    CHECK(dropped > 4'500);
    CHECK(dropped < 5'500);
}

TEST_CASE("the extremes of drop probability are exact, not approximate") {
    const Clean clean;
    fault::set_drop_probability(0.0);
    for (int i = 0; i < 1'000; ++i) REQUIRE_FALSE(fault::should_drop());

    fault::set_drop_probability(1.0);
    for (int i = 0; i < 1'000; ++i) REQUIRE(fault::should_drop());

    CHECK_THROWS_AS(fault::set_drop_probability(-0.1), UsageError);
    CHECK_THROWS_AS(fault::set_drop_probability(1.1), UsageError);
}

TEST_CASE("a partition is symmetric, and only about the nodes named") {
    const Clean clean;
    fault::set_identity("node-a");
    fault::partition("node-a", "node-b");

    CHECK(fault::blocked("node-b"));
    CHECK_FALSE(fault::blocked("node-c"));

    // The same cut, named the other way round. A directional partition is a
    // different experiment and this library does not offer one.
    fault::clear();
    fault::set_identity("node-b");
    fault::partition("node-a", "node-b");
    CHECK(fault::blocked("node-a"));
}

TEST_CASE("a partition between two other nodes does not affect this one") {
    // Unit 8 runs three nodes in one cluster; cutting b from c must leave a
    // talking to both.
    const Clean clean;
    fault::set_identity("node-a");
    fault::partition("node-b", "node-c");
    CHECK_FALSE(fault::blocked("node-b"));
    CHECK_FALSE(fault::blocked("node-c"));
}

TEST_CASE("healing restores traffic, and clears the gate when it was the last fault") {
    const Clean clean;
    fault::set_identity("node-a");
    fault::partition("node-a", "node-b");
    CHECK(fault::any_enabled());

    fault::heal("node-a", "node-b");
    CHECK_FALSE(fault::blocked("node-b"));
    CHECK_FALSE(fault::any_enabled());   // nothing else was on
}

TEST_CASE("a partition with no identity set blocks nothing") {
    // Safer than guessing: a node that has not been told its own name cannot
    // know whether a cut is about it, and silently blocking everything would
    // look like a total outage.
    const Clean clean;
    fault::partition("node-a", "node-b");
    CHECK_FALSE(fault::blocked("node-a"));
    CHECK_FALSE(fault::blocked("node-b"));
}

TEST_CASE("partitions are validated") {
    const Clean clean;
    CHECK_THROWS_AS(fault::partition("", "node-b"), UsageError);
    CHECK_THROWS_AS(fault::partition("node-a", ""), UsageError);
    CHECK_THROWS_AS(fault::partition("node-a", "node-a"), UsageError);
}

TEST_CASE("a hang ends when it is healed, rather than lasting forever") {
    // "hang_forever" is the failure being staged, not a promise about the
    // process. A hang that outlived its own knob would make unit 2's "restart
    // the backend mid-run" unstageable.
    const Clean clean;
    fault::set_hang_forever(true);

    std::thread unhang([] {
        std::this_thread::sleep_for(Millis(80));
        fault::set_hang_forever(false);
    });

    const Stopwatch watch;
    fault::before_response();
    const Nanos elapsed = watch.elapsed();
    unhang.join();

    CHECK(elapsed >= Millis(70));
    CHECK(elapsed < Secs(2));
}

TEST_CASE("clear turns everything off at once") {
    const Clean clean;
    fault::set_latency(Millis(5), Millis(1));
    fault::set_drop_probability(0.5);
    fault::set_identity("node-a");
    fault::partition("node-a", "node-b");
    CHECK(fault::any_enabled());

    fault::clear();
    CHECK_FALSE(fault::any_enabled());
    CHECK_FALSE(fault::should_drop());
    CHECK_FALSE(fault::blocked("node-b"));
}

TEST_CASE("many threads may check and set concurrently") {
    // A target serves on many threads while a control socket flips knobs. TSan
    // checks the claim; the atomics and the one mutex are the mechanism.
    const Clean clean;
    std::atomic<bool> stop{false};
    std::vector<std::thread> readers;
    for (int t = 0; t < 6; ++t) {
        readers.emplace_back([&stop] {
            while (!stop.load(std::memory_order_relaxed)) {
                (void)fault::should_drop();
                (void)fault::blocked("node-b");
                (void)fault::any_enabled();
            }
        });
    }
    for (int i = 0; i < 200; ++i) {
        fault::set_drop_probability((i % 2) == 0 ? 0.5 : 0.0);
        fault::partition("node-a", "node-b");
        fault::heal("node-a", "node-b");
    }
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& reader : readers) reader.join();
    CHECK(true);   // the assertion is TSan's silence
}

// ---------------------------------------------------------------------------
// Chunks 4.2-4.3 — env, the command grammar, and the control socket
// ---------------------------------------------------------------------------

TEST_CASE("each command sets the knob it names") {
    const Clean clean;
    CHECK(fault::apply_command("latency 25 5") == "ok");
    CHECK(fault::any_enabled());
    CHECK(fault::apply_command("clear") == "ok");
    CHECK_FALSE(fault::any_enabled());

    CHECK(fault::apply_command("drop 0.5") == "ok");
    CHECK(fault::any_enabled());
    fault::clear();

    CHECK(fault::apply_command("identity node-a") == "ok");
    CHECK(fault::apply_command("partition node-a node-b") == "ok");
    CHECK(fault::blocked("node-b"));
    CHECK(fault::apply_command("heal node-a node-b") == "ok");
    CHECK_FALSE(fault::blocked("node-b"));

    CHECK(fault::apply_command("status") == "no faults");
    fault::apply_command("drop 1.0");
    CHECK(fault::apply_command("status") == "faults active");
}

TEST_CASE("a command that is wrong is refused, with the verbs listed") {
    const Clean clean;
    CHECK_THROWS_AS(fault::apply_command("lateny 25 5"), UsageError);
    CHECK_THROWS_AS(fault::apply_command("latency 25"), UsageError);       // too few
    CHECK_THROWS_AS(fault::apply_command("latency 25 5 9"), UsageError);   // too many
    CHECK_THROWS_AS(fault::apply_command("drop half"), UsageError);
    CHECK_THROWS_AS(fault::apply_command("drop 2"), UsageError);
    CHECK_THROWS_AS(fault::apply_command("clear now"), UsageError);
    CHECK_THROWS_AS(fault::apply_command(""), UsageError);

    try {
        fault::apply_command("lateny 25 5");
        FAIL("expected a throw");
    } catch (const UsageError& e) {
        // The usual cause is a near miss, so the fix belongs in the message.
        CHECK(std::string(e.what()).find("latency") != std::string::npos);
    }
}

TEST_CASE("a knob can be changed while the target is serving") {
    // Decision 10, which is the whole reason this socket exists. Unit 2 makes
    // a healthy backend hang mid-run; unit 8 partitions a cluster and then
    // heals it. Neither is expressible as startup configuration.
    const Clean clean;
    fault::ControlServer control("127.0.0.1", 0);
    CHECK(control.port() != 0);

    Socket operator_side = Socket::connect_any(
        resolve(Endpoint("127.0.0.1", control.port())), Millis(1000));
    operator_side.set_timeouts(Millis(2000), Millis(2000));

    auto send = [&operator_side](const std::string& line) {
        const std::string out = line + "\n";
        operator_side.write_all(std::span<const char>(out.data(), out.size()));
        char reply[256] = {};
        const std::size_t got = operator_side.read_some(std::span<char>(reply, sizeof(reply)));
        std::string text(reply, got);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        return text;
    };

    CHECK(send("status") == "no faults");
    CHECK(send("drop 1.0") == "ok");
    CHECK(fault::should_drop());          // changed from another process's view
    CHECK(send("status") == "faults active");
    CHECK(send("clear") == "ok");
    CHECK_FALSE(fault::should_drop());
}

TEST_CASE("a malformed command is answered, not a reason to disconnect") {
    // An operator mid-experiment should be able to retype a command rather
    // than reconnect.
    const Clean clean;
    fault::ControlServer control("127.0.0.1", 0);
    Socket operator_side = Socket::connect_any(
        resolve(Endpoint("127.0.0.1", control.port())), Millis(1000));
    operator_side.set_timeouts(Millis(2000), Millis(2000));

    const std::string bad = "nonsense\n";
    operator_side.write_all(std::span<const char>(bad.data(), bad.size()));
    char reply[256] = {};
    std::size_t got = operator_side.read_some(std::span<char>(reply, sizeof(reply)));
    CHECK(std::string(reply, got).rfind("error:", 0) == 0);

    // Still usable.
    const std::string good = "status\n";
    operator_side.write_all(std::span<const char>(good.data(), good.size()));
    got = operator_side.read_some(std::span<char>(reply, sizeof(reply)));
    CHECK(std::string(reply, got).rfind("no faults", 0) == 0);
}

TEST_CASE("the environment is read once, and a bad value is refused") {
    const Clean clean;
    setenv("DARIYANAAP_FAULT_LATENCY_MS", "30", 1);
    setenv("DARIYANAAP_FAULT_JITTER_MS", "5", 1);
    setenv("DARIYANAAP_FAULT_IDENTITY", "node-a", 1);
    setenv("DARIYANAAP_FAULT_PARTITION", "node-a:node-b,node-b:node-c", 1);
    fault::load_from_env();

    CHECK(fault::any_enabled());
    CHECK(fault::blocked("node-b"));
    CHECK_FALSE(fault::blocked("node-c"));   // that cut is between two others

    // A variable set to something unreadable throws rather than being ignored.
    // A typo'd fault variable that silently does nothing produces a run that
    // looks like the experiment and is not it.
    Clean::reset();
    setenv("DARIYANAAP_FAULT_DROP", "half", 1);
    CHECK_THROWS_AS(fault::load_from_env(), UsageError);

    unsetenv("DARIYANAAP_FAULT_LATENCY_MS");
    unsetenv("DARIYANAAP_FAULT_JITTER_MS");
    unsetenv("DARIYANAAP_FAULT_IDENTITY");
    unsetenv("DARIYANAAP_FAULT_PARTITION");
    unsetenv("DARIYANAAP_FAULT_DROP");
}
