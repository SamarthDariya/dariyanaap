#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>
#include <thread>
#include <type_traits>
#include <vector>

#include "core/clock.hpp"
#include "core/endpoint.hpp"
#include "core/errors.hpp"
#include "core/units.hpp"
#include "core/version.hpp"

using namespace dariyanaap;

TEST_CASE("version is reported and non-empty") {
    CHECK_FALSE(version().empty());
}

TEST_CASE("a usage error is catchable as Error, and as a runtime error") {
    CHECK_THROWS_AS(throw UsageError("rate must not be negative"), Error);
    CHECK_THROWS_AS(throw UsageError("rate must not be negative"), std::runtime_error);

    // Not a logic_error: the program is fine, the input was wrong. This is the
    // distinction std::invalid_argument gets backwards for our purposes.
    CHECK_FALSE(std::is_base_of_v<std::logic_error, UsageError>);
}

// ---------------------------------------------------------------------------
// Rate
// ---------------------------------------------------------------------------

TEST_CASE("a rate must be positive, finite, and within the bounds") {
    CHECK_THROWS_AS(Rate::per_second(0.0), UsageError);
    CHECK_THROWS_AS(Rate::per_second(-1.0), UsageError);
    CHECK_THROWS_AS(Rate::per_second(std::numeric_limits<double>::quiet_NaN()), UsageError);
    CHECK_THROWS_AS(Rate::per_second(std::numeric_limits<double>::infinity()), UsageError);
    CHECK_THROWS_AS(Rate::per_second(0.0001), UsageError);  // below kMinRps
    CHECK_THROWS_AS(Rate::per_second(1e10), UsageError);    // above kMaxRps

    CHECK_NOTHROW(Rate::per_second(0.001));
    CHECK_NOTHROW(Rate::per_second(1e9));
}

TEST_CASE("interval is the reciprocal, and the bounds keep it representable") {
    CHECK(Rate::per_second(1000.0).interval() == Micros(1000));
    CHECK(Rate::per_second(0.5).interval() == Secs(2));

    // The bounds exist so llround() never sees an infinity. At the edges the
    // interval is still a sane nanosecond count rather than 0 or an overflow.
    CHECK(Rate::per_second(1e9).interval() == Nanos(1));
    CHECK(Rate::per_second(0.001).interval() == Secs(1000));
}

TEST_CASE("due_at is linear in the index and starts at zero") {
    const Rate r = Rate::per_second(2000.0);
    CHECK(r.due_at(0) == Nanos(0));
    CHECK(r.due_at(1) == Micros(500));
    CHECK(r.due_at(2000) == Secs(1));
}

TEST_CASE("due_at does not drift where accumulating interval() does") {
    // The claim in units.hpp, measured rather than asserted. interval() rounds
    // once; a schedule built by accumulating it adds that same error N times,
    // so the error is systematic and grows linearly.
    const uint64_t n = 3'000'000;

    struct Case { double rps; int64_t drift_ns; };
    // 100k rps drifts nothing because 10,000ns divides a second evenly — which
    // is exactly why this bug survives testing at round rates.
    const Case cases[] = {{3.0, 1'000'000}, {7.0, -428'571}, {100'000.0, 0}};

    for (const Case& c : cases) {
        const Rate r = Rate::per_second(c.rps);
        Nanos accumulated{0};
        for (uint64_t i = 0; i < n; ++i) {
            accumulated += r.interval();
        }
        CHECK((r.due_at(n) - accumulated).count() == c.drift_ns);
    }
}

TEST_CASE("due_at stays accurate far past 2^53 nanoseconds") {
    // Relative precision is what matters here, not exact integer
    // representation: 1e9/1e5 is 10,000ns exactly, and the product stays right
    // even at nine billion requests, where index*1e9 is well past 2^53.
    const Rate r = Rate::per_second(100'000.0);
    for (const uint64_t index : {uint64_t(9'000'000), uint64_t(9'000'000'000)}) {
        CHECK(r.due_at(index).count() == static_cast<int64_t>(index) * 10'000);
    }
}

TEST_CASE("a rate prints as a number an operator would recognise") {
    // to_string(double) would render these as "1000.000000" and "0.500000".
    CHECK(Rate::per_second(1000.0).str() == "1000 rps");
    CHECK(Rate::per_second(0.5).str() == "0.5 rps");
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

TEST_CASE("the clock advances and never runs backwards") {
    const MonotonicClock::Instant start = MonotonicClock::now();
    MonotonicClock::Instant previous = start;
    for (int i = 0; i < 100'000; ++i) {
        const MonotonicClock::Instant current = MonotonicClock::now();
        REQUIRE(current >= previous);  // REQUIRE: one inversion invalidates the rest
        previous = current;
    }
    CHECK(MonotonicClock::since(start) > Nanos(0));
}

TEST_CASE("resolution is positive and finer than the histogram's bottom bucket") {
    const Nanos resolution = MonotonicClock::measured_resolution();
    CHECK(resolution > Nanos(0));

    // Not a flaky assertion — an assumption check. The histogram's bottom
    // bucket is 1us, so if this machine cannot distinguish intervals finer
    // than that, every sub-microsecond latency the rig reports is quantisation
    // noise and the CSV header would have to admit it. Better to fail here.
    CHECK(resolution < Micros(1));
}

TEST_CASE("between and since measure the same interval") {
    const MonotonicClock::Instant a = MonotonicClock::now();
    std::this_thread::sleep_for(Millis(2));
    const MonotonicClock::Instant b = MonotonicClock::now();

    CHECK(MonotonicClock::between(a, b) >= Millis(2));
    // since() reads the clock again, so it can only be longer.
    CHECK(MonotonicClock::since(a) >= MonotonicClock::between(a, b));
}

TEST_CASE("a stopwatch measures the interval since it was made") {
    Stopwatch watch;
    std::this_thread::sleep_for(Millis(5));
    CHECK(watch.elapsed() >= Millis(5));
    CHECK(watch.start() < MonotonicClock::now());
}

TEST_CASE("the clock is safe to read from many threads at once") {
    // The claim TSan is here to check. M1 depends on it: one histogram per
    // thread, merged once at the end, nothing shared on the hot path.
    std::vector<std::thread> threads;
    std::vector<Nanos> elapsed(8, Nanos(0));
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&elapsed, t] {
            Stopwatch watch;
            for (int i = 0; i < 10'000; ++i) {
                (void)MonotonicClock::now();
            }
            elapsed[static_cast<size_t>(t)] = watch.elapsed();
        });
    }
    for (std::thread& t : threads) t.join();
    for (const Nanos e : elapsed) CHECK(e > Nanos(0));
}

// ---------------------------------------------------------------------------
// Endpoint — the type. Parsing arrives in 0.9.
// ---------------------------------------------------------------------------

TEST_CASE("an endpoint that exists is one you could have connected to") {
    const Endpoint e("127.0.0.1", 8080);
    CHECK(e.host() == "127.0.0.1");
    CHECK(e.port() == 8080);

    // The invariants, enforced in the constructor rather than checked by
    // whoever happens to use the value later.
    CHECK_THROWS_AS((Endpoint("", 8080)), InvalidEndpoint);
    CHECK_THROWS_AS((Endpoint("localhost", 0)), InvalidEndpoint);

    // Catchable as a UsageError too, so one CLI handler covers every way the
    // invocation can be wrong.
    CHECK_THROWS_AS((Endpoint("localhost", 0)), UsageError);
    CHECK_THROWS_AS((Endpoint("localhost", 0)), Error);
}

TEST_CASE("the error message names the host, since a run may have several") {
    try {
        Endpoint("backend-3.internal", 0);
        FAIL("expected a throw");
    } catch (const InvalidEndpoint& e) {
        CHECK(std::string(e.what()).find("backend-3.internal") != std::string::npos);
    }
}

TEST_CASE("ipv6 is detected by the colon and printed bracketed") {
    const Endpoint v4("10.0.0.1", 9092);
    CHECK_FALSE(v4.is_ipv6());
    CHECK(v4.str() == "10.0.0.1:9092");

    const Endpoint v6("::1", 9092);
    CHECK(v6.is_ipv6());
    CHECK(v6.str() == "[::1]:9092");

    const Endpoint host("target.internal", 80);
    CHECK_FALSE(host.is_ipv6());
    CHECK(host.str() == "target.internal:80");
}

TEST_CASE("endpoints compare by host and port together") {
    CHECK((Endpoint("a", 1) == Endpoint("a", 1)));
    CHECK_FALSE((Endpoint("a", 1) == Endpoint("a", 2)));
    CHECK_FALSE((Endpoint("a", 1) == Endpoint("b", 1)));
}
