#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <limits>
#include <type_traits>

#include "core/clock.hpp"
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
