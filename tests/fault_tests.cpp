#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <thread>
#include <vector>

#include "core/clock.hpp"
#include "core/errors.hpp"
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
