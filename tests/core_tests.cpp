#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <type_traits>

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
